<!--- Please fill out the following template, which will help other contributors review your Pull Request. -->

<!--- Provide a general summary of your changes in the Title above -->

### Motivation and Context
<!--- Why is this change required? What problem does it solve? -->
<!--- If it fixes an open issue, please link to the issue here. -->
Since #17398 (967b15b88 and 72602f6ad), `zil_commit()` can fail with EIO when the pool suspends with `failmode=continue`, and `zfs_create()`, `zfs_mkdir()` and `zfs_symlink()` return its error on a `sync=always` dataset. They hand the held znode to the caller through `*zpp` before that commit, while every caller takes the znode over only on success, so the error leaks the reference. With `failmode=continue`, a pool that suspends during the commit makes `touch`, `mkdir` or `ln -s` fail with EIO, and then:

- the new entry exists, but a negative dentry hides it (`ls` lists it, `stat` returns ENOENT) until the dentry cache is dropped;
- the inode is never evicted, so once the entry is removed its object stays on the delete queue;
- a later `zfs unmount` or `zpool export` spins forever in `zfsvfs_teardown()`, waiting for `z_all_znodes` to drain.

On FreeBSD, the leaked vnode is also left locked.

### Description
<!--- Describe your changes in detail -->
- Linux and FreeBSD `zfs_create()`, `zfs_mkdir()` and `zfs_symlink()`: set `*zpp` only after the commit succeeded, and release the znode otherwise (unlocking the vnode first on FreeBSD, as the existing error paths do). Callers that already released nothing on error (the zpl and VOP wrappers, ZIL replay, the Linux xattr directory code) are now correct as they are.
- The entry itself is kept. The transaction that created it is already committed and cannot be undone on a suspended pool, and this matches `zfs_write()` and `zfs_fsync()`, which also return EIO for a change that stays applied.
- Linux `zpl_create()`, `zpl_mknod()`, `zpl_mkdir()`, `zpl_symlink()` and `zpl_link()`: `d_drop()` the dentry on error, so that the next lookup asks ZFS rather than trusting a negative dentry for a name that may exist.
- New test `failmode/failmode_create_continue` (Linux): makes each of create, mkdir and symlink fail during the ZIL commit, then checks that the entries are visible without dropping caches, that their objects are freed once removed, and that the pool exports.

Left out, pre-existing and unchanged by this PR:
- The inner error path of the zpl callers (`zfs_remove()` after a failed security label or ACL init) does no `d_drop()`.
- `zfs_remove()`, `zfs_rmdir()` and `zfs_rename()` also return a `zil_commit()` EIO after applying their change, which leaves stale dentries, but they leak no reference.
- The test covers create, mkdir and symlink; it does not cover mknod, link or FreeBSD.

### How Has This Been Tested?
<!--- Please describe in detail how you tested your changes. -->
<!--- Include details of your testing environment, and the tests you ran to -->
<!--- see how your change affects other areas of the code, etc. -->
<!--- If your change is a performance enhancement, please provide benchmarks here. -->
<!--- Please think about using the draft PR feature if appropriate -->
Linux 6.12.112, debug build, in a QEMU (TCG) VM.

- Without the change, `failmode_create_continue` fails at its leak check: the removed objects are still listed by zdb, and the test never reaches its export. On a kernel without the fix, the test's cleanup then spins in `zfsvfs_teardown()` (in the unmount of `zpool destroy`).
- With the change, `failmode_create_continue` passes: each operation returns EIO without blocking, the entries are visible, their objects are freed, and the pool exports.
- With the change, the rest of `tests/functional/failmode` and `slog_replay_fs_001`/`slog_replay_fs_002` (which replay create and mkdir records) were run. In this VM their cleanup cannot remove `/testpool` from a read-only root, so the runner reports them as failures; their own checks passed. The exception is the sync tests (`failmode_{fsync,msync,osync,syncalways}_*`), which clean up before they check, so their checks never ran.
- `slog_replay_fs_002` was run alternately with and without the change, both as the test itself and as a copy of its steps. Across all runs its directory comparison failed once, with the change, and that failure printed no diff. Every other comparison that ran passed, on both builds; two runs reached the 600 s timeout before comparing. `zil_commit_error_count` stayed at 0 in every run, so the changed error paths cannot have been taken.

FreeBSD was not compiled or tested locally; the CI of the fork builds it. The full ZTS was not run.

### Types of Changes
<!--- What types of changes does your code introduce? Put an `x` in all the boxes that apply: -->
- [x] Bug fix (non-breaking change which fixes an issue)
- [ ] New feature (non-breaking change which adds functionality)
- [ ] Performance enhancement (non-breaking change which improves efficiency)
- [ ] Code cleanup (non-breaking change which makes code smaller or more readable)
- [ ] Quality assurance (non-breaking change which makes the code more robust against bugs)
- [ ] Breaking change (fix or feature that would cause existing functionality to change)
- [ ] Library ABI change (libzfs, libzfs\_core, libnvpair and libzfsbootenv)
- [ ] Documentation (a change to man pages or other documentation)

### Checklist
<!--- Go over all the following points, and put an `x` in all the boxes that apply. -->
<!--- If you're unsure about any of these, don't hesitate to ask. We're here to help! -->
- [x] My code follows the OpenZFS [code style requirements](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#coding-conventions).
- [ ] I have updated the documentation accordingly.
- [x] I have read the [**contributing** document](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md).
- [x] I have added [tests](https://github.com/openzfs/zfs/tree/master/tests) to cover my changes.
- [ ] I have run the ZFS Test Suite with this change applied.
- [x] All commit messages are properly formatted and contain [`Signed-off-by`](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#signed-off-by).
