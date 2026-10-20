<!--- Please fill out the following template, which will help other contributors review your Pull Request. -->

<!--- Provide a general summary of your changes in the Title above -->

### Motivation and Context
The `vdev_add` event is posted without a vdev, so it carries no `vdev_guid`, `vdev_path` or other detail of what was added, unlike `vdev_attach`, `vdev_remove` and the other vdev events.

Closes #7107

### Description
`spa_vdev_add()` now builds one event per added top-level vdev with `spa_event_create()`, the same way the other vdev events get their `vdev_guid`, `vdev_state`, `vdev_path`, `vdev_devid` and friends, and posts them where the single event used to be, after the pool configuration is updated. The events are created in the loop that moves the new top-level vdevs under the root vdev, because the temporary vdev holding them is freed by `spa_vdev_exit()`.

I chose one event per vdev rather than one event with all of them because every consumer reads single-valued members (`ZEVENT_VDEV_GUID`, `ZEVENT_VDEV_PATH`, ...) and the vdev events elsewhere work that way. Adding a single vdev, the usual case, gives the same event as before plus the vdev members. A top-level mirror, raidz or dRAID vdev has a `vdev_guid` but no `vdev_path`. Spares and cache devices are not top-level vdevs and are not described, also when added together with other vdevs; adding only spares or cache devices still posts one event without a vdev, as before. They could be described too in a follow up.

`vdev_add` is not handled by any zedlet or by the zed agents, so zed itself behaves as before and the new members show up in `ZEVENT_*` for the generic zedlets. `zpool-events.8` is updated.

### How Has This Been Tested?
Linux 6.12.112, debug build, QEMU TCG VM.

- New test `events/vdev_add_event`: adds two top-level vdevs with one `zpool add`, checks from `zpool events -v` that there is one `vdev_add` event per vdev with the right `vdev_guid` and `vdev_path`, that the ZED log has the same `ZEVENT_VDEV_GUID` and `ZEVENT_VDEV_PATH` values, and that adding a mirror gives one event with its guid and no path, and that adding only a spare still posts one event. It fails on master (the events carry no vdev) and passes with this change.
- `events_001_pos`, which checks the `vdev_add` events of a spare and of a log device, passes. In my VM the root file system is read-only, so I ran it with `-m none` added to its `zpool create` and with a writable `/usr/local` for the ZED.
- Everything builds, including libzpool and ztest.

Not run: the full ZFS Test Suite, `events_002_pos` and the other zed tests, FreeBSD.

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
