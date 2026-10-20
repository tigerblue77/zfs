### Motivation and Context
A sequential resilver (`zpool replace -s`, `zpool attach -s`) can cancel itself when the vdevs are reopened while it runs. Nothing restarts it: the replacing vdev stays, and `zpool wait` never returns. Every `zpool scrub` reopens all vdevs before it checks whether it may start, so a scrub that is refused because the resilver is running is enough. That is how `functional/replacement/scrub_cancel` hung on a Fedora 44 VM of my fork's CI, in a run for an unrelated change, until it was killed after 600 seconds: its sequential iteration refuses a scrub right after `zpool replace -s`, and the `zpool wait` that follows never returned.

Once per metaslab, `vdev_rebuild_thread()` asks `vdev_rebuild_should_cancel()`, that is `vdev_resilver_needed()`, whether a leaf still needs the rebuild. `vdev_resilver_needed()` only counts writeable leaves, and the thread holds only `SCL_CONFIG`. `vdev_reopen()` runs under `spa_vdev_state_enter()`, with `SCL_STATE_ALL` held as writer: it closes every leaf, then opens them again, through a task queue created for each interior vdev. A check that lands in between finds the new leaf closed, and the thread cancels the rebuild. Replaying `scrub_cancel` in userspace (libzpool, file vdevs), the rebuild was canceled and the replace hung once in 152 iterations.

`vdev_rebuild_initiate_sync()` and `vdev_rebuild_reset_sync()` have the same window, with a `VERIFY(vdev_resilver_needed(...))`: a reopen that overlaps them panics the system. They run in syncing context, which holds `SCL_CONFIG` but no longer `SCL_STATE` once `spa_sync()` has gone through the dirty state list. In the kernel, the attach that initiates a rebuild holds `spa_namespace_lock` until its txg has synced, which keeps out the commands that reopen, unless they had opened the pool before the attach started. A reset is requested later, by the rebuild thread, after `zpool attach -s` to a vdev that is already rebuilding, and any `zpool scrub` or `zpool reopen`, or `zpool online` of a device of that top-level vdev, can overlap it. In userspace, with the reopen of `zpool scrub` repeated by a thread a few milliseconds apart, both VERIFYs fail, and not rarely; see below.

### Description
- `vdev_rebuild_should_cancel()` takes `SCL_STATE` as reader around `vdev_resilver_needed()`, as `vdev_rebuild_range()` already does before its own `vdev_writeable()` check. Its only caller is the rebuild thread, which holds `SCL_CONFIG` and not `vdev_rebuild_lock` there, so the lock order does not change.
- `vdev_rebuild_initiate_sync()` and `vdev_rebuild_reset_sync()` take `SCL_STATE` as reader before `vdev_rebuild_lock`, the order used by `vdev_rebuild_range()`, and by `spa_vdev_attach()`, which calls `vdev_rebuild()` with `SCL_ALL` held; while they wait for it, they hold no other lock than `SCL_CONFIG` and `dp_config_rwlock`. Syncing context already waits for `SCL_STATE` as reader at the start of every `spa_sync()` and in `spa_sync_rewrite_vdev_config()`, and sync tasks such as `spa_change_guid_check()` and `raidz_reflow_scratch_sync()` take it with `dp_config_rwlock` held; `spa_vdev_state_exit()` drops the lock before it waits for a txg. So the sync tasks add no new wait. This is a separate commit.
- New test `functional/replacement/rebuild_reopen`: four file vdevs of 1 GiB with `VDEV_MIN_MS_COUNT=64`, so that the rebuilt vdev has 63 metaslabs and as many checks; `zpool replace -s` with the resilver suspended, and a refused scrub as in `scrub_cancel`; then four background loops of `zpool scrub` while the resilver runs. It fails if the history shows a canceled rebuild, if the resilver is still running after 120 seconds, or if the replacing vdev is still there 60 seconds later. It replaces the device three times each way, then checks that the pool is online without errors.
- `vdev_rebuild_should_stop()` also reads `vdev_writeable()` of the top-level vdev without `SCL_STATE`, in the `zfs_scan_suspend_progress` loop of `vdev_rebuild_ranges()`. There, a reopen only ends that wait early, and `vdev_rebuild_range()` checks again under `SCL_STATE_ALL` before it does anything, so I left it.
- Not covered: a leaf that really stops being writeable (offlined, faulted, removed) between the request of a rebuild and its sync task would still fail the VERIFYs. I have not tried to reach that.
- Two open pull requests touch the same code. #19179 changes `vdev_rebuild_should_cancel()` so that it no longer updates the saved txg range; the two conflict there only in text. #19445 has `vdev_rebuild_cancel_sync()` request a healing resilver when a vdev still needs one: as far as I can tell from reading it, the replace would then complete after this race through a healing resilver instead of hanging, but the sequential resilver would still be canceled.

### How Has This Been Tested?
Linux x86_64, GCC, debug build, userspace only (libzpool, file vdevs in tmpfs), two to four runs in parallel. The master column uses master with counters added, which can also count the failures of the two VERIFYs instead of asserting them (the "counted" figures); the other figures come from the code of this branch.

| test | master | this change |
| --- | --- | --- |
| replay of `rebuild_reopen`: same sizes, tunables, four loops of `zpool scrub` (pool stats, then scan request, 1 to 3 ms apart), bounded waits, history check | failed in 12 of 12 runs, each at its first rebuild, canceled | passed in 8 of 8 runs, of 6 rebuilds each, in 73 to 86 seconds; with the first commit alone, 12 of 12, in 72 to 77 seconds |
| `zpool attach -s` to an idle two-way mirror, then detach, while a thread repeats the reopen of `zpool scrub` 0 to 20 ms apart | with the first commit alone, the VERIFY of `vdev_rebuild_initiate_sync()` stopped 4 of 4 runs within 30 attaches; counted instead of asserted, it failed 12 times in 80 attaches, each time with the only leaf missing data closed | no failure and no hang in 120 attaches, 4 runs of 30 |
| a rebuild kept running by `zfs_scan_suspend_progress`, then `zpool attach -s` and detach of another device, while the same thread runs 0 to 5 ms apart | counted instead of asserted, the VERIFY of `vdev_rebuild_reset_sync()` failed 2 times in 600 resets, each time with the only leaf missing data closed | no failure and no hang in 600 resets, 4 runs of 150 |

In the one failure of the attach runs that I timestamped, the reopening thread had asked for the lock 2.4 ms before it got it, and the check ran 0.3 ms after it got it, while it was reopening.

`make unit`, `make commitcheck`, `cstyle`, `testscheck`, `spdxcheck`, `vcscheck`, two 4-minute ztest runs and a build of `vdev_rebuild.c` without `--enable-debug` with `-Werror` pass. `shellcheck` reports on the new test only the notices it reports on the other tests of the directory (SC1091, SC2068, SC2086); `make shellcheck` does not cover `.ksh` tests. The CI of my fork: RESULT.

Not run: the ZFS Test Suite with the kernel module, the new test and `functional/replacement` included, on Linux or FreeBSD; I have not run it locally.

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
