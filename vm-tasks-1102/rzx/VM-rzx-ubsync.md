# VM task: RAID-Z expansion, i/o issued between spa_uberblock and spa_ubsync

You are on a disposable Debian 13 (trixie) x86_64 VM. You may reload modules and
reboot. Do not post anything anywhere: no GitHub comment, issue, PR or push.
Put every result under `~/rzx/results/` and finish with `~/rzx/results/REPORT.md`.

Take a VM snapshot first if you can. No pool other than the ones this task creates
may exist (`zpool list` must print "no pools available" before you start). ZTS
destroys pools whose name contains `testpool` and overwrites its loop devices.

## What is being checked

At master 0cdf531257:
- `raidz_reflow_sync()` (vdev_raidz.c:4303) commits copy progress into `spa_uberblock`
  under the expansion range lock (writer).
- `vdev_raidz_io_start()` (vdev_raidz.c:2846-2853) maps i/o with the offset in
  `spa_ubsync`.
- `spa_sync()` copies `spa_uberblock` into `spa_ubsync` at its end (spa.c:11489),
  without the lock.

An i/o that starts in between uses the old location of the range just committed. If its
writes are still in flight when `spa_ubsync` advances, they can overwrite data that the
reflow copies there next. The fix (`0001-raidz-take-the-reflow-range-lock-when-spa_ubsync-advances.patch`)
takes the range lock while `spa_ubsync` is updated.

This was reproduced in userspace only (libzpool). Results from the host:
- `zio_rewrite()` in the window: master damaged 3/3 (permanent error), fix clean 3/3.
- Read with a column repair in the window: master and master+#19433 damaged; fix+#19433 clean 3/3.

There is no deterministic kernel reproducer: holding one specific leaf write needs the
userspace `pwrite64()` interposer. In the kernel, this task checks three things:
1. The window is reached with real i/o in flight, using a dbgmsg-only observation patch
   on master and on the fix.
2. The fix does not hang or regress, with the expansion under load and with ZTS `-T raidz`.
3. The userspace reproducer still gives the same before/after result on this VM.

## Files (copy the whole directory to the VM as ~/rzx/in)

- `0001-raidz-take-the-reflow-range-lock-when-spa_ubsync-advances.patch`: the fix (`git am`).
- `vm-observe-master.diff`: master only. It logs `rzx-ubsync: txg N advances spa_ubsync
  with i/o in flight in [a, b)` when a writer `tryenter` on that range fails. Behaviour
  is otherwise unchanged.
- `vm-instrumentation.diff`: on top of the fix. It logs `rzx-ubsync: txg N waited U us
  for i/o in [a, b)` when the new lock had to wait.
- `repro/rzub.c`, `repro/build.sh`, `repro/run.sh`: the userspace reproducer.
- `repro/pr19433-3343d58f9.diff`: #19433's change, needed only for the read mode.

## 0. Packages and sources (as root)

```sh
set -e
apt-get update
apt-get install -y build-essential autoconf automake libtool gawk git \
    dkms linux-headers-$(uname -r) libblkid-dev uuid-dev libudev-dev \
    libssl-dev zlib1g-dev libaio-dev libattr1-dev libelf-dev libffi-dev \
    libtirpc-dev libcurl4-openssl-dev python3 python3-setuptools \
    python3-cffi python3-packaging ksh bc fio acl attr sysstat mdadm \
    lsscsi parted pax gdb sudo
mkdir -p ~/rzx/results
cd ~/rzx
git clone https://github.com/openzfs/zfs zfs-master
cd zfs-master && git checkout --detach 0cdf531257 && cd ..
git -C zfs-master worktree add --detach ../zfs-fix 0cdf531257
cd zfs-fix
git -c user.name=t -c user.email=t@t am ~/rzx/in/0001-*.patch
cd ..
for t in zfs-master zfs-fix; do
    (cd $t && ./autogen.sh && ./configure --enable-debug && \
        make -j$(nproc)) > results/build-$t.log 2>&1
done
tail -2 results/build-zfs-*.log
```

Expected: both builds end without error. Record `uname -r` and
`git -C zfs-fix log --oneline -2` in REPORT.md.

## 1. Userspace reproducer on this VM (before/after)

```sh
cd ~/rzx
mkdir -p repro && cp in/repro/* repro/
for t in master fix; do repro/build.sh ~/rzx/zfs-$t $t; done
for i in 1 2 3; do
    repro/run.sh master write $i; repro/run.sh fix write $i
done | tee results/userspace-write.txt
grep -h "^scrub\|^bad block\|^RESULT\|spa_ubsync" repro/runs/*/out.txt \
    > results/userspace-write-details.txt
```

Each run takes about 3 minutes and about 700 MB in `repro/runs/`. `run.sh` deletes the
device files after each run. Expected: every `master write` run ends
`RESULT: DAMAGED`, with 1 permanent error log entry and one `bad block` line, and its
output says "spa_ubsync advanced ... with the write in flight". Every `fix write` run
says "spa_ubsync stays at txg ... while the write is in flight" and ends `RESULT: CLEAN`.
If a run exits 1 ("no window holding a candidate block"), run it again and note it.

Optional, read mode (needs #19433):
```sh
cd ~/rzx
git -C zfs-master worktree add --detach ../zfs-pr 0cdf531257
git -C zfs-master worktree add --detach ../zfs-prfix 0cdf531257
(cd zfs-pr && git apply ~/rzx/in/repro/pr19433-3343d58f9.diff)
(cd zfs-prfix && git apply ~/rzx/in/repro/pr19433-3343d58f9.diff && \
    git apply ~/rzx/in/0001-*.patch)
for t in zfs-pr zfs-prfix; do
    (cd $t && ./autogen.sh && ./configure --enable-debug && \
        make -j$(nproc)) > results/build-$t.log 2>&1
done
for t in pr prfix; do repro/build.sh ~/rzx/zfs-$t $t; done
for i in 1 2 3; do repro/run.sh pr read $i; repro/run.sh prfix read $i; done \
    | tee results/userspace-read.txt
```
Expected: `pr read` damaged (checksum errors on one child after the scrub, no
permanent error), `prfix read` clean.

## 2. Kernel: is the window reached with i/o in flight? (before/after)

Apply the observation patches and rebuild. Only spa.c changes.
```sh
cd ~/rzx
git -C zfs-master apply ~/rzx/in/vm-observe-master.diff
git -C zfs-fix apply ~/rzx/in/vm-instrumentation.diff
for t in zfs-master zfs-fix; do (cd $t && make -j$(nproc)) \
    > results/rebuild-$t.log 2>&1; done
```

Then run the workload below once with each tree's modules, master first:
```sh
cd ~/rzx/zfs-master      # second pass: cd ~/rzx/zfs-fix
./scripts/zfs.sh -u 2>/dev/null; ./scripts/zfs.sh
echo 1 > /sys/module/zfs/parameters/zfs_dbgmsg_enable
echo 0 > /proc/spl/kstat/zfs/dbgmsg
echo 0 > /sys/module/zfs/parameters/zfs_scrub_after_expand
D=/var/tmp/rzxdev; rm -rf $D; mkdir -p $D
for i in 1 2 3 4 5; do truncate -s 2G $D/d$i; done
./zpool create -f -o ashift=12 rzxpool raidz1 $D/d1 $D/d2 $D/d3 $D/d4
./zfs create -o recordsize=128k -o compression=off rzxpool/fs
fio --name=fill --directory=/rzxpool/fs --rw=write --bs=1M --size=3G \
    --numjobs=1 --end_fsync=1 > /dev/null
# sync writes (ZIL) and reads during the expansion
fio --name=zil --directory=/rzxpool/fs --rw=randwrite --bs=8k --size=256M \
    --fsync=1 --numjobs=4 --time_based --runtime=3600 > /dev/null &
fio --name=rd --directory=/rzxpool/fs --filename=fill.0.0 --rw=randread \
    --bs=128k --numjobs=2 --time_based --runtime=3600 > /dev/null &
./zpool attach -w rzxpool raidz1-0 $D/d5
kill %1 %2; wait
./zpool scrub -w rzxpool
./zpool status -v rzxpool > ~/rzx/results/kernel-$(basename $PWD)-status.txt
grep "rzx-ubsync" /proc/spl/kstat/zfs/dbgmsg \
    > ~/rzx/results/kernel-$(basename $PWD)-dbgmsg.txt
wc -l ~/rzx/results/kernel-$(basename $PWD)-dbgmsg.txt
./zpool destroy rzxpool; rm -rf $D
./scripts/zfs.sh -u
```
If `zfs.sh -u` fails, reboot, then continue with the other tree.

Record for each tree: the number of `rzx-ubsync` lines, the largest wait (fix tree),
how long `zpool attach -w` took, and the error counters in `zpool status`.

Expected, but not certain because it depends on timing: some
`advances spa_ubsync with i/o in flight` lines on master, and `waited ... us` lines on
the fix. Zero lines on both means this workload did not reach the window; say so. No
checksum errors are expected in either run: this workload does not delay any write, so
there is nothing to land late. The master run is not expected to show corruption.

## 3. Kernel: ZTS `-T raidz` on the fix

Use the fix tree without the instrumentation patch:
```sh
cd ~/rzx/zfs-fix && git checkout module/zfs/spa.c && make -j$(nproc) \
    > ~/rzx/results/rebuild-fix-clean.log 2>&1
git log --oneline -1; git status --short
```
ZTS refuses to run as root, so use a user with passwordless sudo:
```sh
id zts 2>/dev/null || useradd -m zts
echo 'zts ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/zts
cp -a ~/rzx/zfs-fix /home/zts/zfs-fix && chown -R zts: /home/zts/zfs-fix
su - zts -c 'cd zfs-fix && sudo ./scripts/zfs-helpers.sh -i && \
    sudo ./scripts/zfs.sh && \
    ./scripts/zfs-tests.sh -vx -T raidz' > ~/rzx/results/zts-raidz-fix.log 2>&1
tail -40 ~/rzx/results/zts-raidz-fix.log
```
For every FAIL or KILLED test, rerun it alone on the fix (`-t <path>`) and on master
(`git -C ~/rzx/zfs-master checkout module/zfs/spa.c`, rebuild, copy, the same way).
Report whether the result is the same without the fix. Copy the log directory that
the run names (`/var/tmp/test_results/...`) into `~/rzx/results/`.

Watch for a hang (a test KILLED on timeout, `zpool attach -w` that never returns). If
one happens, save `cat /proc/spl/kstat/zfs/dbgmsg`, `echo t > /proc/sysrq-trigger` +
`dmesg` and the stacks of `txg_sync` (`cat /proc/$(pgrep txg_sync)/stack`).

## 4. REPORT.md

Short, facts only:
- VM kernel, both commit ids.
- Section 1: the table of runs and their RESULT lines.
- Section 2: counts and largest wait per tree, attach duration, status counters.
- Section 3: pass/fail summary, every failure and whether it also fails on master,
  any hang with its stacks.
- What did not run and why.

Nothing is posted anywhere.
