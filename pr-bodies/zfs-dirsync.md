<!--- Please fill out the following template, which will help other contributors review your Pull Request. -->

<!--- Provide a general summary of your changes in the Title above -->

### Motivation and Context
<!--- Why is this change required? What problem does it solve? -->
<!--- If it fixes an open issue, please link to the issue here. -->
Closes #4366

On Linux, `dirsync` is accepted as a mount option and `zfs mount -o dirsync` passes it down as `MS_DIRSYNC`, but it has no effect: namespace operations only reach the ZIL before returning with `sync=always`, which also makes every write synchronous. Applications that need durable, ordered directory updates but can leave file writes asynchronous (the distributed systems use case given in the issue) have nothing between `sync=standard` and `sync=always`. As discussed in the issue, this adds a dedicated `dirsync` property, wired like `relatime`, rather than a new `sync` value.

### Description
<!--- Describe your changes in detail -->
- New inheritable boolean property `dirsync`, default `off`, appended at the end of `zfs_prop_t`. There is no on-disk format change and no feature flag.
- When it is on, `zfs_create` (also used for mknod), `zfs_mkdir`, `zfs_rmdir`, `zfs_remove`, `zfs_rename`, `zfs_link` and `zfs_symlink` call `zil_commit(zilog, <directory object>)` after a successful operation, next to the existing `sync=always` calls, on Linux and FreeBSD. The commit is for the directory's object rather than `0`, so asynchronous writes to other files are not pushed out with each directory update. Linking an `O_TMPFILE` file keeps its current behaviour (it waits for the txg and is not logged).
- On Linux, `IS_DIRSYNC(dir)` is also honoured, so mounting with `dirsync` or `sync` (legacy `mount -t zfs -o dirsync`, `zfs mount -o dirsync`) now makes directory updates synchronous whatever the property says. The mount option does not change the property's value, and `zfs get` does not report it as a temporary value.
- Following ext4 (`fs/ext4/namei.c` checks `IS_DIRSYNC()` in `ext4_add_nondir()` (create, mknod, symlink), `ext4_mkdir()`, `ext4_rmdir()`, `__ext4_unlink()`, `__ext4_link()` and both rename paths; `ext4_setattr()` and the xattr code only check `IS_SYNC()`), attribute changes are not affected, and neither are operations in hidden extended attribute directories, since setting an extended attribute does not change the namespace. `chattr +D` is not supported, as it would need a per-file on-disk flag.
- `sync=disabled` keeps priority, since `zil_commit()` returns immediately then. This is documented.
- A rename also commits the renamed file's pending writes, since `zil_itx_assign()` moves them to the sync list for `TX_RENAME`. This is documented.
- Behaviour change on Linux: existing mounts with `-o sync` or `-o dirsync` get synchronous directory operations from now on, which costs performance on namespace-heavy workloads. That is what these options mean on other filesystems.
- FreeBSD: the property works the same way there (same call sites in `zfs_vnops_os.c`). `MNT_SYNCHRONOUS` (`mount -o sync`) is not mapped to it.
- If `zil_commit()` fails (`failmode=continue`), the dirsync branch handles the error exactly as the `sync=always` branch beside it does: it assigns the error and returns it. Both branches share a pre-existing leak, which is not introduced here: on Linux, `zfs_create`/`zfs_mkdir`/`zfs_symlink` return the error with `*zpp` still held, and `zpl_create`/`zpl_mkdir`/`zpl_symlink` only release it on success.
- `zfsprops.7` documents the property, its interaction with `sync=disabled` and with the Linux mount options; `zfs-allow.8` lists it among the delegable properties.
- `libzfs.abi`: only the `zfs_prop_t` enum changes (`ZFS_PROP_DIRSYNC` = 108, `ZFS_NUM_PROPS` = 109). I compared that block with the one `abidw` 2.4.0 writes for the built `libzfs.so` and they are identical. I did not commit a regenerated file, because a full `make storeabi` with this libabigail version rewrites thousands of unrelated lines.
- An older `zfs receive` given a stream that carries the property (`zfs send -p`) prints `cannot receive dirsync property on <fs>: invalid property value` and finishes the receive with exit status 0. The kernel rejects the unknown name with `EINVAL` and records it in the error list (`module/zfs/zfs_ioctl.c`, `zfs_set_prop_nvlist()`, lines 3215-3223 and 3282-3283, called from `zfs_ioc_recv_impl()` at line 5897 with its return value ignored). libzfs reports it and carries on (`lib/libzfs/libzfs_sendrecv.c:5124-5126`, `lib/libzfs/libzfs_util.c:599-601`). I checked this by receiving such a stream with an unpatched build.

### How Has This Been Tested?
<!--- Please describe in detail how you tested your changes. -->
<!--- Include details of your testing environment, and the tests you ran to -->
<!--- see how your change affects other areas of the code, etc. -->
<!--- If your change is a performance enhancement, please provide benchmarks here. -->
<!--- Please think about using the draft PR feature if appropriate -->
Linux 6.12.112, debug build, in a QEMU TCG VM (2 vCPU). New ZTS tests in `tests/functional/dirsync`. The first three count the per-dataset `zil_commit_count` kstat before and after each operation (create, mkfifo, mkdir, link, symlink, rename, unlink, rmdir):

- `dirsync_ops`: no commit with the default `dirsync=off`, a commit for every operation with `dirsync=on`, none with `dirsync=on` and `sync=disabled`.
- `dirsync_inherit`: a child inherits the property and its value takes effect; a local override and `zfs inherit` work; `zfs send -p | zfs receive` keeps it (source `received`) and it takes effect on the received file system.
- `dirsync_mount` (Linux only): a legacy mount with `-o dirsync` and with `-o sync`, and `zfs mount -o dirsync`, commit for every operation; the same mounts without the option do not.

- `dirsync_replay` checks durability, not just that `zil_commit()` was called. It creates a pool on a file vdev, with entries to remove and rename, and writes a ZIL header with an `fsync`. It then runs `zpool freeze`, so no later transaction group writes an uberblock or label (`vdev_config_sync()` skips txgs past the freeze txg, and frees are not reused). Next it runs the operations and copies the vdev file while the pool is still imported. The copy is read through the same page cache that the file vdev writes go through, and `sync` is avoided, because it would commit the ZIL. Finally it exports the original, deletes it and imports the copy, whose ZIL is replayed. Export cannot be used directly, because `zil_close()` commits everything. With `dirsync=off`, the test also checks that `zil_commit_count` did not move, then that every operation is lost. With `dirsync=on`, every operation is replayed.

All four tests pass with this change. All four fail on unpatched master (7c3d85a):
- `dirsync_ops`, `dirsync_inherit` and `dirsync_replay` fail because the property does not exist. Note that `dirsync_replay`'s `dirsync=off` half is exactly master's behaviour: the operations are lost.
- `dirsync_mount` fails on its first operation, because nothing is committed.

For the master run, the tests sourced `dirsync.kshlib` from the patched tree, because master's suite does not have it. The test logic was the same.

Ad hoc checks on the patched build: with `xattr=dir` and `dirsync=on`, setting or removing an extended attribute and `chmod` do not commit, and a create does. After `mount -o dirsync`, `zfs get dirsync` still shows `off default`. `mount -o remount,dirsync` does not set the flag (Linux leaves `MS_DIRSYNC` out of `MS_RMT_MASK`), and `zfs mount -o remount,dirsync` fails with `EINVAL`. Unpatched master behaves the same way in both cases.

Also: `cstyle.pl`, `mancheck.sh`, `spdxcheck.pl` on the changed files, `make testscheck`, `make shellcheck`, `git diff --check`, `commitcheck.sh`. `make checkabi` fails only on `strlcat`/`strlcpy`, which show up as removed from libnvpair, libzfs_core and libzfs. This build host's glibc provides those functions, so the failure comes from the environment and not from this change. Without the `.abi` update, `abidiff` also reports the `zfs_prop_t` enumerator change; with it, it does not.

Not tested: FreeBSD, which was not compiled locally; the fork CI's FreeBSD jobs will build it and run the common tests, including `dirsync_replay`. Also not tested: the full ZTS, a release build, and remounting with `dirsync` through the new mount API.

### Types of Changes
<!--- What types of changes does your code introduce? Put an `x` in all the boxes that apply: -->
- [ ] Bug fix (non-breaking change which fixes an issue)
- [x] New feature (non-breaking change which adds functionality)
- [ ] Performance enhancement (non-breaking change which improves efficiency)
- [ ] Code cleanup (non-breaking change which makes code smaller or more readable)
- [ ] Quality assurance (non-breaking change which makes the code more robust against bugs)
- [ ] Breaking change (fix or feature that would cause existing functionality to change)
- [x] Library ABI change (libzfs, libzfs\_core, libnvpair and libzfsbootenv)
- [x] Documentation (a change to man pages or other documentation)

### Checklist
<!--- Go over all the following points, and put an `x` in all the boxes that apply. -->
<!--- If you're unsure about any of these, don't hesitate to ask. We're here to help! -->
- [x] My code follows the OpenZFS [code style requirements](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#coding-conventions).
- [x] I have updated the documentation accordingly.
- [x] I have read the [**contributing** document](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md).
- [x] I have added [tests](https://github.com/openzfs/zfs/tree/master/tests) to cover my changes.
- [ ] I have run the ZFS Test Suite with this change applied.
- [x] All commit messages are properly formatted and contain [`Signed-off-by`](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#signed-off-by).
