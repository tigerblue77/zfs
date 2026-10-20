<!--- Please fill out the following template, which will help other contributors review your Pull Request. -->

<!--- Provide a general summary of your changes in the Title above -->

### Motivation and Context
<!--- Why is this change required? What problem does it solve? -->
<!--- If it fixes an open issue, please link to the issue here. -->
`zfs destroy` has no way to say what kind of dataset the caller expects, so a script that means to destroy a snapshot destroys a file system or a volume as soon as the `@snap` part of the name is lost. #9522 asks for a type gate in the spirit of `zfs list -t`.

This continues #15959 by Chris Simons, which implemented the option, was reviewed by @amotin (asking to unify its type parser with the one of `zfs list`) and has been idle since September 2024. The commit credits him with a `Co-authored-by` trailer, with the name and address of his commit.

Closes #9522

### Description
<!--- Describe your changes in detail -->
`zfs destroy` gets `-t type[,type]...`. The types are the ones `zfs list -t` accepts: `filesystem`, `volume`, `snapshot`, `bookmark`, `all` and the aliases `fs`, `vol`, `snap`. If the type of the argument (a dataset, a snapshot specification or a bookmark) is not in the list, the command prints `cannot destroy '<name>': type does not match '<list>'` and exits with status 1 before anything is destroyed, unshared or unmounted. An unknown type is a usage error (status 2), like an unknown option. Without `-t` the behaviour is unchanged.

Only the argument is checked. With `-r` or `-R` the children, snapshots and clones of other types that the command destroys anyway are still destroyed along with it: the option guards against naming the wrong thing, it is not a filter on what a recursive destroy removes. The man page says so.

Compared to #15959:
- the snapshot and bookmark forms (`-d`, ranges and comma-separated lists of snapshots) are covered, and the type is checked after the argument is classified, so a snapshot specification no longer needs to be opened as a dataset;
- a comma-separated list of types and `all` are accepted, as in `zfs list`;
- the usage text shows the option for each of the three forms, and the man page documents it in each of them;
- the type list parser is now one function, shared by `zfs destroy`, `zfs list` and `zfs get`, which carried identical copies (the review comment on #15959); it no longer modifies the argument string, and the messages and behaviour of `zfs list` and `zfs get` are unchanged;
- two ZTS tests: `zfs_destroy_017_pos` and `zfs_destroy_018_neg`, which also checks that a mismatched type destroys nothing and leaves the file system mounted.

The bash completion of `zfs destroy` offers `-t` and the type names, as it does for `zfs list` and `zfs get`. The change is in `cmd/zfs`, the man page, the completion script and the tests; it does not touch libzfs or any ABI.

### How Has This Been Tested?
<!--- Please describe in detail how you tested your changes. -->
<!--- Include details of your testing environment, and the tests you ran to -->
<!--- see how your change affects other areas of the code, etc. -->
<!--- If your change is a performance enhancement, please provide benchmarks here. -->
Debug build on Linux 6.12.112, run in a QEMU (TCG) VM, on top of this commit and, for the failing runs, on unmodified master (7c3d85a):
- `zfs_destroy_017_pos` and `zfs_destroy_018_neg` fail on master (`invalid option 't'`, exit status 2) and pass with this change;
- the other `cli_root/zfs_destroy` tests, 001 to 016, were run with the change: all pass except `zfs_destroy_015_pos`, which fails at `zpool create testpool2 /var/tmp/disk` in the VM, at the same step and with the same error on unmodified master, before anything that involves this change;
- `cli_user/zfs_list` 001 to 005, 007 and 008, and `cli_root/zfs_list/zfs_list_009_pos` pass;
- `cli_root/zfs_get/zfs_get_009_pos`, the one that uses `zfs get -t`, was cut off by its time limit after 30 minutes in the VM, with all of the 358 steps it had reached succeeding; the `-t` steps at its end were not reached;
- since that left the shared parser under-tested, I ran `zfs list` and `zfs get` with 21 values of `-t` (names, aliases, lists, empty and invalid elements) in four forms, with the binaries of master and of this commit on the same pool: output and exit status are identical in all 84 comparisons;
- the `.in` file of the bash completion passes `bash -n` and ShellCheck, with no finding, as before; the completion itself was not exercised in an interactive shell.

`./scripts/cstyle.pl -cpP cmd/zfs/zfs_main.c`, `./scripts/mancheck.sh man/man8/zfs-destroy.8`, `git diff --check` against master, `./scripts/spdxcheck.pl` and `./scripts/commitcheck.sh` pass. I did not run the rest of the ZFS Test Suite, the livelist and device removal tests of `zfs_destroy`, a release build, `make checkstyle` as a whole or `make checkabi`, nor test on FreeBSD (the C change uses nothing platform specific).

### Types of Changes
<!--- What types of changes does your code introduce? Put an `x` in all the boxes that apply: -->
- [ ] Bug fix (non-breaking change which fixes an issue)
- [x] New feature (non-breaking change which adds functionality)
- [ ] Performance enhancement (non-breaking change which improves efficiency)
- [x] Code cleanup (non-breaking change which makes code smaller or more readable)
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
- [ ] I have run the ZFS Test Suite with this change applied.
- [x] All commit messages are properly formatted and contain [`Signed-off-by`](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#signed-off-by).
