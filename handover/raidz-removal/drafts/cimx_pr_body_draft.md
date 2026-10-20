### Motivation and Context
Closes #19492.

Jobs that land on GitHub's ubuntu24 runner image 20261018 fail while installing their dependencies:

```
E: Unable to locate package linux-modules-extra-7.0.0-1017-azure
```

That image boots kernel 7.0.0-1017-azure, for which noble-updates has `linux-modules-7.0.0-1017-azure` but no `linux-modules-extra`, nor for any other 7.0 Azure kernel. Besides the qemu-x86 jobs reported in #19492 (`qemu-1-setup.sh`), the unit-tests, smatch and ZFS ARM build jobs fail the same way in `qemu-3-deps-vm.sh`, which they run on the runner with `ONLY_DEPS=1`, for example https://github.com/tigerblue77/zfs/actions/runs/39273975919/job/118133346576.

### Description
Both scripts install `linux-modules-extra-$(uname -r)` only when `apt-cache` knows the package. Nothing changes on image 20261012 or in the ubuntu22 and ubuntu24 VMs. The ubuntu26 case already does not install it.

### How Has This Been Tested?
`bash -n` passes on both scripts, and `shellcheck` reports no new finding (one SC2046 fewer in each). On Ubuntu 24.04 with the current noble-updates, `apt-cache show linux-modules-extra-7.0.0-1017-azure` fails and `apt-cache show linux-modules-extra-6.8.0-31-generic` succeeds. The CI of my fork: RESULT.

### Types of Changes
- [ ] Bug fix (non-breaking change which fixes an issue)
- [ ] New feature (non-breaking change which adds functionality)
- [ ] Performance enhancement (non-breaking change which improves efficiency)
- [ ] Code cleanup (non-breaking change which makes code smaller or more readable)
- [x] Quality assurance (non-breaking change which makes the code more robust against bugs)
- [ ] Breaking change (fix or feature that would cause existing functionality to change)
- [ ] Library ABI change (libzfs, libzfs\_core, libnvpair and libzfsbootenv)
- [ ] Documentation (a change to man pages or other documentation)

### Checklist
- [x] My code follows the OpenZFS [code style requirements](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#coding-conventions).
- [ ] I have updated the documentation accordingly.
- [ ] I have read the [**contributing** document](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md).
- [ ] I have added [tests](https://github.com/openzfs/zfs/tree/master/tests) to cover my changes.
- [ ] I have run the ZFS Test Suite with this change applied.
- [x] All commit messages are properly formatted and contain [`Signed-off-by`](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#signed-off-by).
