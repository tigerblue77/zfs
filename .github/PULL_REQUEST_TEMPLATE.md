<!--- Please fill out the following template, which will help other contributors review your Pull Request. -->

<!--- Provide a general summary of your changes in the Title above -->

### Motivation and Context
<!--- Why is this change required? What problem does it solve? -->
<!--- If it fixes an open issue, please link to the issue here. -->

### Description
<!--- Describe your changes in detail -->

### How Has This Been Tested?
<!--- Please describe in detail how you tested your changes. -->
<!--- Include details of your testing environment, and the tests you ran to -->
<!--- see how your change affects other areas of the code, etc. -->
<!--- If your change is a performance enhancement, please provide benchmarks here. -->
<!--- Please think about using the draft PR feature if appropriate -->

### Types of Changes
<!--- What types of changes does your code introduce? Put an `x` in all the boxes that apply: -->
- [ ] Bug fix (non-breaking change which fixes an issue)
- [ ] New feature (non-breaking change which adds functionality)
- [ ] Performance enhancement (non-breaking change which improves efficiency)
- [ ] Code cleanup (non-breaking change which makes code smaller or more readable)
- [ ] Quality assurance (non-breaking change which makes the code more robust against bugs)
- [ ] Breaking change (fix or feature that would cause existing functionality to change)
- [ ] Library ABI change (libzfs, libzfs\_core, libnvpair and libzfsbootenv)
- [ ] Documentation (a change to man pages or other documentation)

### Priority
<!--- For a bug fix, how soon should it be reviewed? Put an `x` in one box. -->
<!--- It is read once, when the PR is opened; a maintainer may change it. -->
- [ ] Critical (data loss or corruption, silently wrong data included, on a pool that was healthy)
- [ ] High (a pool that cannot be imported, a panic or deadlock in normal use, a security issue, or a regression in a release)
- [ ] Medium (as high, but only in an uncommon setup, after existing on-disk damage, or in a race not seen outside a test)
- [ ] Low (messages, tool output, debug-only assertions)

### Checklist
<!--- Go over all the following points, and put an `x` in all the boxes that apply. -->
<!--- If you're unsure about any of these, don't hesitate to ask. We're here to help! -->
- [ ] My code follows the OpenZFS [code style requirements](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#coding-conventions).
- [ ] I have updated the documentation accordingly.
- [ ] I have read the [**contributing** document](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md).
- [ ] I have added [tests](https://github.com/openzfs/zfs/tree/master/tests) to cover my changes.
- [ ] I have run the ZFS Test Suite with this change applied.
- [ ] All commit messages are properly formatted and contain [`Signed-off-by`](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#signed-off-by).
