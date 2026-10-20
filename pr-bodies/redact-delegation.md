<!--- Please fill out the following template, which will help other contributors review your Pull Request. -->

<!--- Provide a general summary of your changes in the Title above -->

### Motivation and Context
`zfs redact` is registered with `zfs_secpolicy_config()`, so only root can run it, unlike most other ioctls which can be delegated with `zfs allow`. This adds a delegatable permission for it.

An earlier commit by @tshowalter that is linked from the issue (not submitted as a pull request) took a similar approach but checked the permission only on the redacted dataset. This change also requires it on the dataset of each redaction snapshot, since redact reads them. Thanks for the earlier work; I did not use that code.

Closes #10296

### Description
A new `redact` permission, checked by a new `zfs_secpolicy_redact()`. It is required on the dataset of the snapshot being redacted, which is also where the redaction bookmark is created, and on the dataset of each snapshot in the redaction list (checked on the filesystem, like the other snapshot permissions, so a local-only grant is enough), whose blocks are compared against the redacted snapshot. Root in the global zone keeps working as before. The old check, `zfs_secpolicy_config()`, refused root in a Linux non-global zone and required `PRIV_ZFS_POOL_CONFIG` in a FreeBSD jail; the new one is the usual `secpolicy_zfs()` and zone check, so root in a container or jail can now redact the zoned datasets it can see, as with send and bookmark. A user still needs the `send` permission to use the resulting bookmark in a redacted send.

Other changes needed to declare a permission: the name in `dsl_deleg.h` and `zfs_deleg_perm_tab`, a `ZFS_DELEG_NOTE_REDACT` value and the matching entry and comment in `zfs allow` output, and the entry in `zfs-allow.8`. The new `zfs_deleg_note_t` value is added right before `ZFS_DELEG_NOTE_NONE`, so existing values do not change; `libzfs.abi` is updated for the new enumerator, its terminator and the size of `zfs_deleg_perm_tab` (no exported function changes). `zfs-allow.8` also notes that `redact` creates the bookmark without the `bookmark` permission and that the redaction list is metadata not charged to any quota.

I considered requiring the permission only on the redacted dataset. I went with checking the redaction snapshots too because they are read to build the redaction list, which seemed the more conservative default; it is easy to relax if reviewers prefer.

### How Has This Been Tested?
Linux 6.12.112, debug build, QEMU TCG VM.

- New test `delegate/zfs_allow_redact`: a user with other related permissions (send, snapshot, bookmark, clone, create, mount) cannot redact, a user with `redact` can, a grant that is local to the redacted dataset (`zfs allow -l`) is enough, and the permission can be removed again. It uses a new `verify_fs_redact` in `delegate_common.kshlib`, which also checks that `redact` on only one of the two datasets is not enough. On master the test fails (`zfs allow` rejects `redact` as an invalid permission); with this change it passes.
- `redact` added to the permission list of `zfs_allow_010_pos`. That test takes more than the 10 minute limit in my emulated VM, so I ran a copy of it reduced to the `redact` (and `send`) entries, which passed. I did not run the complete test.
- By hand as an unprivileged user: `zfs redact` fails with EPERM with other permissions only, with `redact` on the redacted dataset only, succeeds with `redact` on both, and `zfs send --redact` as root still works. Root `zfs redact` with a missing snapshot still fails as before.
- `make checkabi` shows no difference for `zfs_deleg_note_t`; it reports `strlcpy`/`strlcat` removed from libzfs, which comes from this build environment's libc, not from this change.

Not run: the full ZFS Test Suite, the redacted_send tests (the VM cannot mount the pools they create because its root is read-only), FreeBSD.

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
- [x] I have read the [**contributing** document](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md).
- [x] I have added [tests](https://github.com/openzfs/zfs/tree/master/tests) to cover my changes.
- [ ] I have run the ZFS Test Suite with this change applied.
- [x] All commit messages are properly formatted and contain [`Signed-off-by`](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#signed-off-by).
