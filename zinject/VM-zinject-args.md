# VM task: ZTS zinject_args on a mirror, master against the fix (zts-zinject-args-mirror-read)

Self-contained task for a **disposable Debian 13 VM** (kernel 6.12, no pool
on it that matters, reboots allowed). Nothing is posted anywhere; results
stay on the VM and are copied back by hand. About 1.5 to 2 h: two builds,
then 40 runs of about a minute each. No reboot unless a run hangs.

## What is being tested

`tests/zfs-tests/tests/functional/cli_root/zinject/zinject_args.ksh`
(added by #18579) creates a mirror of every disk in `$DISKS`, three loop
devices by default. Its last pass, `verify_injection`, injects read errors
on the first disk only (`zinject -d $DISK1 -e io -T read -f 100`), reads
one 128K block once with `primarycache=none`, and fails with
`device-fault injection did not execute (inject=0)` whenever the mirror
reads that block from another child:

- a normal mirror read goes to one child only:
  `vdev_mirror_child_select()` takes the least loaded readable child, and
  among children of equal load picks `(io_offset >> 21) % <tied children>`
  (`vdev_mirror_preferred_child_randomize()`); another child is read only
  if the first one fails;
- a device injection matches only the leaf that receives the I/O
  (`zio_vdev_io_done()` calls `zio_handle_device_injections()` for that
  leaf).

The delay check after it (`-D 5:1 -T write` on `$DISK1`) cannot miss: a
mirror sends every write to every child. Reported: 3 passes in 6 reruns on
one fork branch in CI, and in #19635 1 failure in 3 runs on master, 2 in 4
with that change.

The fix, fork branch **`zts-zinject-args-mirror-read`**, head
**`9ae72311c375062354bff005fb591d4a72a5f696`** (one commit on master
`3028351741106e28af57961a97d81c760a73fd44`), registers the read fault on
every child (`for disk in $DISKS`) and checks the sum of the inject counts,
so whichever child serves the read, its handler fires. The read then fails
outright instead of being repaired from another child; the test already
ignores its status. It changes the test file only: both trees build the
same module and tool sources, apart from the git revision string compiled
in.

Established on the development host, from reading the code only: the cause
above, and the delay check being unaffected. The changed file passes
`ksh -n` and `bash -n`, shellcheck reports the same findings as on master's
file, and the new `awk` sum was checked on a sample `zinject` listing.
**Not established, the point of this task**: any run of either version in
a kernel.

## Safety rules

- Disposable VM only. `zinject-args.sh` creates a user `zts` with
  passwordless sudo (ZTS refuses to run as root and needs sudo), unloads
  and loads the trees' `spl` and `zfs` modules, and ZTS creates and
  destroys a pool `testpool` on three loop devices over 4 GB sparse files
  under `/var/tmp/za/files`. Its own cleanup destroys only pools named
  `testpool*` and detaches only loop devices backed by files under that
  directory.
- Do not use `zfs-tests.sh -x` here: besides every `testpool*` pool it
  removes every device-mapper device that is not open
  (`dmsetup remove_all`), whoever it belongs to.
- Run as root. Post nothing. Never run two `zinject-args.sh` at once.
- Every step has a bound in the script. A run that hangs past it is logged
  as `TIMEOUT`; if what it left cannot be cleaned, the script prints
  `STOP ... reboot, rerun`: save `dmesg` and
  `/proc/spl/kstat/zfs/dbgmsg`, reboot, and start `run` again (it resumes
  after the last logged run).

## Files needed on the VM

Fork branch `vm-tasks-1103b` of https://github.com/tigerblue77/zfs carries
this document and `zinject/zinject-args.sh`:

```sh
mkdir -p /root/za && cd /root/za
timeout 600 git clone -q --depth 1 -b vm-tasks-1103b https://github.com/tigerblue77/zfs.git vmtasks
cp vmtasks/zinject/zinject-args.sh /root/za/zinject-args.sh && chmod +x /root/za/zinject-args.sh
sha256sum /root/za/zinject-args.sh   # must be 8eec361e7b6df1f22977ae0502f93dccf28e11df076d484782e66de44e6c7b84
```

## 1. Prepare the VM (once, about 10 min)

```sh
export DEBIAN_FRONTEND=noninteractive
timeout 1800 apt-get update -y
timeout 3600 apt-get install -y build-essential git autoconf automake \
    libtool libtool-bin gawk alien fakeroot dkms bc cpio libaio-dev \
    libattr1-dev libblkid-dev libcurl4-openssl-dev libelf-dev libffi-dev \
    libpam0g-dev libssl-dev libtirpc-dev libudev-dev uuid-dev zlib1g-dev \
    python3 python3-cffi python3-dev python3-packaging python3-setuptools \
    kmod "linux-headers-$(uname -r)" ksh sudo fio acl attr file parted \
    lsscsi util-linux
lsmod | grep -wE 'zfs|spl'; zpool list 2>/dev/null   # must show nothing
```

## 2. Sources and build (about 40-60 min)

```sh
timeout 18000 /root/za/zinject-args.sh setup; echo "setup rc=$?"
```

`setup` creates the user `zts` and `/etc/sudoers.d/za-zts`, clones the fix
branch twice under `/var/tmp/za/src` as `zts` (`--depth 2`), checks out
master (the branch's parent) in `src/master`, checks both heads and that
only the fix has the new loop, then builds each tree as `zts`:
`./autogen.sh && ./configure --enable-debug && make -j$(nproc)`, logged to
`/var/tmp/za/build-master.log` and `build-fix.log`. It checks the modules
and tools exist and `ZFS_DEBUG` is defined, loads each tree's modules once
(srcversion of the loaded `zfs` = that tree's `module/zfs.ko`,
`/proc/sys/kernel/spl/gitrev` = that tree's `include/zfs_gitrev.h`,
`(DEBUG mode)` in the load message), and unloads them.

Expected: `setup rc=0`, every `CHECK` PASS, `INFO build master rc=0`,
`INFO build fix rc=0`, and two `INFO tree` lines with different git
revisions. Failure signatures: a clone or checkout failure (network, or
the branch moved: `head_fix` FAIL), a build error
(`grep -E 'error:|undefined' /var/tmp/za/build-*.log | head`), a load
check FAIL (`dmesg | tail -n 50`). Run `setup` again after fixing the
cause: it keeps what is done.

## 3. The 40 runs (about 30-45 min)

```sh
timeout 7200 /root/za/zinject-args.sh run; echo "run rc=$?"
/root/za/zinject-args.sh summary
```

Odd runs are master, even runs the fix (1 master, 2 fix, ..., 40 fix).
Each run: cleans what a killed run could leave (`zinject -c all`, pools
`testpool*`, loop devices over `/var/tmp/za/files`, the vdev files);
unloads the modules, loads the tree's own and checks them as `setup` did;
then runs, as `zts`, from the tree:

```sh
./scripts/zfs-tests.sh -v -d /var/tmp/za/files \
    -t tests/functional/cli_root/zinject/zinject_args.ksh
```

under `timeout -k 60 1500` (ZTS itself kills the test after 600 s). The
result is the last field of the test's `Test: ... [PASS|FAIL|KILLED]`
line. It logs `RUN <n> <tree> <result> rc=<zfs-tests.sh status> s=<seconds>
dmesg_new=<n> log=<results dir>` to `/var/tmp/za/za.log`, copies the
results directory to `/var/tmp/za/run/<n>-<tree>.results`, keeps the
console output in `/var/tmp/za/run/<n>-<tree>.out`, and for a failed run
logs each line of the test log matching
`ERROR|FAIL|KILLED|did not execute|inject=` as `FAILLINE`. New `dmesg`
lines matching `VERIFY|ASSERT|PANIC|BUG:|Oops|WARNING:|hung_task|blocked
for more|soft lockup|Call Trace` are logged as `DMESG` and the whole `dmesg`
saved as `<n>-<tree>.dmesg`. If the outer `timeout` or a `STOP` ends the
loop, start `run` again: it resumes after the last `RUN` line.

`summary` prints, per tree, the PASS count and the range of run times,
then every non-PASS `RUN` line, every `RUN` line with a non-zero status or
new `dmesg` lines, and every `FAILLINE`, `DMESG`, `STOP`, failed `CHECK`
and `INFO` line.

Expected: master fails some of its 20 runs (7 passes in the 13 reported
runs above), each failure with the `FAILLINE`
`device-fault injection did not execute (inject=0)`; the fix passes 20 of
20, with `rc=0` and `dmesg_new=0` throughout. Failure signatures: on the
fix, `device-fault injection did not execute (inject=0)` means the read
reached no disk at all (served from a cache), `object-fault ...` or
`delay injection did not execute` another cause in the later checks; on
either tree, any other `FAILLINE`, `TIMEOUT`, `KILLED` or `NORESULT`
(read that run's `.out`), or a `DMESG` line.

## 4. Cleanup (about 1 min)

```sh
timeout 900 /root/za/zinject-args.sh clean; echo "clean rc=$?"
```

`clean` removes what `run` could leave and unloads the modules. Once the
evidence is copied off the VM, the rest can go too:

```sh
userdel -r zts; rm -f /etc/sudoers.d/za-zts; rm -rf /var/tmp/za /var/tmp/test_results
```

## Evidence to collect

`/var/tmp/za/za.log`, the output of `zinject-args.sh summary`, the tail of
`/var/tmp/za/build-*.log` and every `error:`/`warning:` line of them, the
`.out` file and `.results` directory of every run that did not pass,
every `.dmesg` file, and `dmesg > /root/za/dmesg-final.txt`.

## Report format

```
VM: <name>, Debian 13, kernel <uname -r>, <nproc> CPUs, <RAM>
setup rc=<0|n>; build master rc=<n>, fix rc=<n>; checks <n> PASS, <n> FAIL
master 3028351741: <n>/20 PASS, <min>-<max> s per run
fix    9ae72311c3: <n>/20 PASS, <min>-<max> s per run
Every non-PASS RUN line and every FAILLINE, DMESG and STOP line of
summary, verbatim; anything skipped and why.
```
