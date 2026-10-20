<!--- Please fill out the following template, which will help other contributors review your Pull Request. -->

<!--- Provide a general summary of your changes in the Title above -->

### Motivation and Context
<!--- Why is this change required? What problem does it solve? -->
<!--- If it fixes an open issue, please link to the issue here. -->
A volume's block device always reports 512-byte logical sectors; only the physical block size follows `volblocksize`. Consumers that need 4 KiB sectors (an iSCSI target exporting the volume, a guest image formatted with 4 KiB sectors that one wants to inspect on the host) cannot get them. The patches posted in the issue set the logical block size from `volblocksize` unconditionally, which would change the geometry of every existing volume, breaking partition tables and guest file systems, and would fail for the default 16K `volblocksize`, which is larger than a page.

Closes #4203

### Description
<!--- Describe your changes in detail -->
This adds a `volsectorsize` volume property that sets the logical sector size of the device:

- It can only be set at creation time (`PROP_ONETIME_DEFAULT`, like `volblocksize` it is read-only afterwards), since changing it under an existing partition table or file system would make them unreadable. `zfs set`, `zfs inherit`, `zfs clone -o`, `zfs receive -o` and `zfs receive -x` refuse it, as they do `volblocksize`.
- Valid values are powers of two from 512 B to 4 KiB, and no larger than `volblocksize`, which stays the physical block size. 4 KiB is the largest size every supported block layer and page size accepts. libzfs checks this with readable messages; `zfs_ioc_create()` checks it again for other callers, and minor creation refuses a value outside that range read from disk instead of handing it to the block layer.
- The default is 512 B, and a volume created without the property behaves exactly as today.
- Like `volsize`, the value is stored in the volume's ZAP object (`ZVOL_ZAP_OBJ`), and only for volumes created with the property. Snapshots, clones and received copies keep it through any send stream (no `-p` needed), and older implementations ignore the entry and expose the volume with 512-byte sectors, as now (see the question below).
- A forced receive of a full stream (`zfs receive -F`) could replace an existing volume with one of another sector size. Its block device cannot change its sector size safely while it may be opened: recreating it races with any opener (udev, multipathd, a VM), which could see the new data through the old geometry and would block the removal. So `dmu_recv_end_check()` refuses such a receive, open volume or not, in syncing context, with a new `ZFS_ERR_VOLSECTORSIZE_MISMATCH`. libzfs reports it as "the stream's volume sector size differs from that of <volume>: destroy the volume first, or receive it under another name". No resume state or `%recv` clone is kept. If the sector sizes cannot be read, the check is skipped and noted in `zfs_dbgmsg`. Receives that keep the sector size, including onto an open volume, behave as on master. `zfs-receive(8)` and `zfsprops(7)` document the refusal. The new error code is added at the end of `zfs_errno_t` and in pyzfs's list; `zfs_errno_t` appears in no `.abi` file, and `abidw` output for all four libraries is unchanged by it.
- Linux sets the queue's `logical_block_size` (in `struct queue_limits` on 6.9+, with `blk_queue_logical_block_size()` before). FreeBSD uses it for the GEOM provider's `sectorsize` and for `DIOCGSECTORSIZE` in `volmode=dev`. In `volmode=dev`, `zvol_cdev_read()` and `zvol_cdev_write()` still accept I/O aligned to 512 bytes, while `DIOCGSECTORSIZE` reports the sector size; this change adds no alignment checks there, since that code is not compiled or tested here.
- `zfs get`, `zfs list -o volsector` and channel programs (`zfs.get_prop`) report it. `zcp_get.c` reads the ZAP entry itself, like `volsize` next to it, because it is also built into libzpool, which does not build `zvol.c`; `dmu_recv.c` does the same for the check above. `lib/libzfs/libzfs.abi` gains only the new `zfs_prop_t` enumerator: running `abidw` (2.4.0, with the options of `make storeabi`) on `libzfs.so` built from master and from this branch gives outputs that differ by exactly that enumerator, so that hunk is what the file gains.

Alternatives left out, open to discussion:
- Setting the logical size from `volblocksize` or the physical size unconditionally (the patches in the issue): breaks existing volumes and fails above a page.
- A settable property applied when the device is next created (import, rename, `volmode` change): it could change the geometry of a disk in use without warning, and a plain send would lose it.
- A module parameter: global rather than per volume, with the same problem for existing volumes.
- An index property such as `default | physical`: less general, and `physical` would be invalid for most `volblocksize` values.

Pre-existing, not changed here: a forced receive over a volume of another `volblocksize` leaves the existing device with the old physical block size until the next import or `volmode` change, because `zv_volblocksize` is only read when the minor is created. Refusing that case would change existing behaviour, so it is left as is.

Question for maintainers: this adds no feature flag. An older implementation still reads and writes every block of such a volume correctly; only the sector size its device presents differs, as with other presentation-only changes (`volmode`, `snapdev`, the change of the default `volblocksize`), which had none. A read-only compatible feature would not prevent the misread either: an older system would still import the pool read-only and show the volume with 512-byte sectors, where a partition table or file system made with 4 KiB sectors is not found. `zfsprops(7)` warns about this. Is that acceptable, or would you rather see a feature?

### How Has This Been Tested?
<!--- Please describe in detail how you tested your changes. -->
<!--- Include details of your testing environment, and the tests you ran to -->
<!--- see how your change affects other areas of the code, etc. -->
<!--- If your change is a performance enhancement, please provide benchmarks here. -->
<!--- Please think about using the draft PR feature if appropriate -->
This branch is based on master 1e9b428dc. All runs are on Linux 6.12.112 with a debug build (`--enable-debug`), in a QEMU TCG VM (2 vCPUs, 4 GB, x86_64). The guest root file system is read-only, so every directory's `cleanup` script fails there and is left out below.

New test `zvol/zvol_misc/zvol_misc_sectorsize` (Linux only, since it reads the sizes with `blockdev`):
- Without this change: FAIL, at the first `zfs create -o volsectorsize=...`, as expected. This ran on a build of master 7c3d85a, an earlier base of this branch; nothing between 7c3d85a and 1e9b428dc touches volumes' sector size.
- With this change: PASS. It covers:
  - the default (512 logical, `volblocksize` physical) and each valid size from 512 B to 4 KiB;
  - refusal of invalid values, of a size above `volblocksize`, of file systems, and of `zfs set`, `zfs inherit` and `zfs clone -o`;
  - an O_DIRECT write and read back through 4 KiB sectors, and a 512-byte O_DIRECT read on that device, which must fail;
  - a clone and a received copy that keep the sector size;
  - a forced receive of a 4 KiB volume over a 512-byte one, refused with the expected message whether the device is open or not, with `-s`: no resume token, no snapshot and no `%recv` dataset (checked with `zdb -d`) are left, and the device keeps 512-byte sectors; a forced receive over an open volume of the same sector size still works;
  - the sizes after `zpool export` and `zpool import`.

Existing tests:
- On this revision: every test of `zvol/zvol_misc` (002_pos, hierarchy, rename_inuse, snapdev, trim, volmode, zil, fua, rename_open, rename_write), and the new test, all pass.
- On an earlier revision of this commit, which differs only in the forced-receive handling, the man pages, pyzfs's error list and the new test:
  - `channel_program/synctask_core`: tst.get_number_props (updated for the new property), tst.list_system_props, tst.get_index_props, tst.get_string_props and tst.get_neg pass.
  - `cli_root/zfs_create`: zfs_create_002_pos, 006_pos, 009_neg, zfs_create_dryrun and zfs_create_verbose pass.
  - `cli_root/zfs_set`: property_alias_001_pos, zfs_set_002_neg and zfs_set_003_neg pass. ro_props_001_pos and zfs_set_001_neg were killed by the 30-minute test timeout: in this emulated VM each `zfs` command takes seconds, and both had logged about 400 checks, all as expected, when they were stopped. Neither covers the new property.
- `cli_root/zfs_get`, on that earlier revision before the rebase onto 1e9b428dc:
  - zfs_get_002_pos, 003_pos, 006_neg, 007_neg, 008_pos and 010_neg pass.
  - zfs_get_004_pos fails in this VM because it creates pools with a mountpoint under the read-only `/`.
  - zfs_get_001_pos, 005_neg and 009_pos were killed by the 20-minute test timeout with no failure logged; they check fixed property lists that do not include the new one.

By hand in the same VM:
- Forced receives on this revision: a different sector size is refused with the message above, whether the volume is open or closed, and leaves no resume token or `%recv` dataset; the same sector size onto an open volume is received, as on master, where a forced receive onto an open volume works.
- On that earlier revision:
  - the sector size survives export/import, rename, `volmode=dev` and `snapdev=visible` (a snapshot device has the volume's sector size);
  - `zfs send -p` and `zfs send -R` streams keep it;
  - `zfs receive -o volsectorsize=...` and `-x volsectorsize` are refused, as they are for `volblocksize`;
  - growing `volsize` keeps it;
  - `zfs create -n -v`, `zfs list -o volsector`, `zfs get -j` and `zfs program` (`zfs.get_prop`) report it;
  - `zdb` shows the `sectorsize` entry in the ZAP object only for volumes created with the property.
- No kernel warning was logged in any run.

Also checked on this revision:
- `cstyle.pl -cpP` on the changed C files, `mancheck.sh` on `zfsprops.7` and `zfs-receive.8`, `git diff --check 1e9b428dc`, `commitcheck.sh`, and `spdxcheck.pl` on the new test.
- ABI: I ran `abidw`, with the options of `make storeabi`, on the four libraries built from master 7c3d85a and from this revision. The only difference is the new `zfs_prop_t` enumerator in libzfs, which is exactly the change to `libzfs.abi`. On an earlier revision, `abidiff` of the committed `libzfs.abi` against the built `libzfs.so` (as `make checkabi` does) reported only the `strlcpy`/`strlcat` difference that master shows on the same host, whose glibc provides them.

Not tested:
- FreeBSD: the GEOM and `DIOCGSECTORSIZE` changes are not even compiled.
- Kernels other than 6.12.112: the `blk_queue_logical_block_size()` path, for kernels without `struct queue_limits` at disk allocation, was neither compiled nor run.
- Other architectures and page sizes, a release build, and the full ZTS.
- Use with an iSCSI target or a guest.

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
