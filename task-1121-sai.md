TASK-1121-SAI (from the coordinator session "main"): functional, about 45 min with the build. Make the pool image that the regression test of our PR #19985 ("Linux: don't log the size change while replaying TX_SETATTR") imports: a 64 MiB file vdev written by OpenZFS 2.4.4 on Debian 13 (6.12), whose log holds exactly one `TX_SETATTR` record that carries a size change and a mode, left unreplayed by `zpool freeze`. Report its sha256, the zdb lines of the record and the compressed image itself, in base64. One release build, no ZTS, nothing measured: other VMs may run functional tasks at the same time. The test runs come later in TASK-1121-SAT, on this same VM, which reads the image from `$L`: keep `$L` as it is at the end.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-sai`, `L=/var/log/task-1121-sai`, `Z=$D/zfs` (the 2.4.4 tree), `M=$D/img` (the image and its mount points).

## Why

#19706 made Linux `zfs_setattr()` log a size change as `TX_TRUNCATE`, from `zfs_freesp()`, instead of in `TX_SETATTR`. It also made `zfs_freesp()` log while the ZIL is replayed: replaying a `TX_SETATTR` that carries `ATTR_SIZE` then marks the record replayed twice, once in the transaction of the size change and once in the one that sets the other attributes, possibly in two txgs. If the first txg reaches disk and the system stops before the second, the next replay skips the record and its mode and times are lost. #19985 fixes that. Its test needs such a record, which current Linux code no longer writes, but 2.4.4 (before #19706) does: its `zfs_setattr()` logs its whole mask in `TX_SETATTR`, `ATTR_SIZE` included.

The record is made by a truncation of a setuid file by a user without `CAP_FSETID` (`nobody`): the kernel then clears the setuid bit in the same setattr call, so the record carries `ATTR_MODE` (0x1) and `ATTR_SIZE` (0x8) besides `ATTR_MTIME` (0x20) and `ATTR_CTIME` (0x40), plus `ATTR_FILE` (0x2000) from `ftruncate()`: mask 0x2069 expected. Freezing the pool before it leaves the record in the log, unreplayed, as a crash would.

Note on zdb: on Linux, zdb decodes `lr_mask` with the Solaris `AT_*` values of userland (`AT_MODE` 0x2, `AT_GID` 0x8, `AT_SIZE` 0x80; our #19685 fixes that), so for mask 0x2069 2.4.4's zdb is expected to print only `AT_GID <n>` under the record, and no `AT_MODE`, `AT_SIZE` or `AT_MTIME` line. The raw mask on the `foid ..., mask 0x...` line is what counts here.

What is built: https://github.com/openzfs/zfs tag `zfs-2.4.4` (tag object `f75f3256a0916c5ba090b088e6ea5a9528f6f956`), commit `71a9f9578616a90c3c14bb59629fb4d31bfd68d1`, tree `d2a62ec4527f872ce840ba60f50248e66fcc080b`. Release build (`./configure` without `--enable-debug`): the image does not depend on it, and it is faster.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- Touch no block device of the VM and no pool this task did not create. The only pool of this task is `testpool_setattr`, on the file `$M/setattr_size.dat` and, in step 5, on a copy of it under `$M/check`; it is created with `-R` and imported with `-o cachefile=none`, so `/etc/zfs/zpool.cache` never names it. Never `zpool import` anything else, never `zpool import -a`, never `zfs-tests.sh` in this task.
- The VM must be on Debian 13 (`uname -r` starts with `6.12.`): if not, stop and report.
- The build runs detached (`setsid nohup ... &`) and is polled; a hang is the build still going after 60 minutes, or any `zpool`/`zfs` command of step 4 or 5 not returning within 5 minutes: capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 100` into `$L/hang.txt`, then stop and report. No reboot is approved in this task.
- If a commit, a tree or an expected output differs, stop and report the outputs as they are; do not retry the image steps on your own (each run gives a different image).
- `sudo -n fstrim -av` after deleting the tree. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 10 GiB free on /var/tmp: one tree), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `id nobody`, `ls -d $D $L 2>&1` (both absent), `ls -l /etc/zfs/zpool.cache 2>&1` (absent expected; size 0: move it to `$L/zpool.cache.step0` once `$L` exists in step 1 and say so; above 0: stop and report its `ls -l`, load nothing), `command -v bzip2 base64 sha256sum truncate setpriv`.

## STEP 1: the tree

```sh
D=/var/tmp/task-1121-sai; L=/var/log/task-1121-sai; Z=$D/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L
git clone -q https://github.com/openzfs/zfs $Z && cd $Z
git checkout -q zfs-2.4.4
echo "$(git rev-parse zfs-2.4.4) $(git rev-parse HEAD) $(git rev-parse HEAD^{tree}) $(git status --porcelain | wc -l)"
#   f75f3256a0916c5ba090b088e6ea5a9528f6f956 71a9f9578616a90c3c14bb59629fb4d31bfd68d1 d2a62ec4527f872ce840ba60f50248e66fcc080b 0
grep -n 'err = zfs_freesp(zp, vap->va_size, 0, 0, FALSE);' module/os/linux/zfs/zfs_vnops_os.c
#   2102:		err = zfs_freesp(zp, vap->va_size, 0, 0, FALSE);
grep -n 'zfs_log_setattr(zilog, tx, TX_SETATTR, zp, vap, mask, fuidp);' module/os/linux/zfs/zfs_vnops_os.c
#   2611:		zfs_log_setattr(zilog, tx, TX_SETATTR, zp, vap, mask, fuidp);
```

Stop and report if a value differs. If the clone fails, wait 1 minute and try once more.

## STEP 2: build (about 20 minutes)

```sh
D=/var/tmp/task-1121-sai; L=/var/log/task-1121-sai; Z=$D/zfs
setsid nohup sh -c "cd $Z && ./autogen.sh && ./configure && nice make -j$(nproc); echo build-rc=\$?" > $L/build.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build.log`: it must end `build-rc=0`; quote every `warning:` line outside objtool notes (none expected). Note S = `modinfo -F srcversion $Z/module/zfs.ko`.

## STEP 3: load

`sudo -n $Z/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S) and the `ZFS: Loaded module v2.4.4-1` line of dmesg (no "(DEBUG mode)": release). Note N0 = `sudo -n dmesg | wc -l`. `sudo -n $Z/zpool list -H -o name` must print nothing.

## STEP 4: the image

Run these blocks one after the other, each line's rc checked; any rc other than 0, or an output other than the one shown, is a stop.

```sh
D=/var/tmp/task-1121-sai; L=/var/log/task-1121-sai; Z=$D/zfs; M=$D/img
mkdir -p $M && truncate -s 64M $M/setattr_size.dat
sudo -n $Z/zpool create -R $M/alt testpool_setattr $M/setattr_size.dat; echo create-rc=$?
sudo -n $Z/zfs create testpool_setattr/fs; echo fs-rc=$?
F=$M/alt/testpool_setattr/fs
yes abcdefgh | head -c 131072 | sudo -n tee $F/file > /dev/null; echo write-rc=$?
sudo -n chmod 4777 $F/file; echo chmod-rc=$?
sudo -n -u nobody test -w $F/file && echo NOBODY-CAN-WRITE
# The log header must exist before the freeze, or nothing logged after it reaches disk.
sudo -n dd if=/dev/zero of=$F/sync bs=1 count=1 conv=fsync status=none; echo dd-rc=$?
sudo -n $Z/zpool sync testpool_setattr; echo sync-rc=$?
stat -c '%n %s %a %U:%G %y' $F/file $F/sync
#   .../file 131072 4777 root:root <date>
#   .../sync 1 644 root:root <date>
```

Then the frozen part. Run it as one block, without pause:

```sh
D=/var/tmp/task-1121-sai; Z=$D/zfs; M=$D/img; F=$M/alt/testpool_setattr/fs
sudo -n $Z/zpool freeze testpool_setattr; echo freeze-rc=$?
sudo -n -u nobody truncate -s 4096 $F/file; echo truncate-rc=$?
sudo -n dd if=/dev/null of=$F/file bs=1 count=0 conv=notrunc,fsync status=none; echo fsync-rc=$?
stat -c '%n %s %a %U:%G %Y %y' $F/file
#   .../file 4096 777 root:root <seconds> <date with nanoseconds>
sudo -n $Z/zpool export testpool_setattr; echo export-rc=$?
sha256sum $M/setattr_size.dat
```

Quote the whole output. Note T4 = the `%y` field of the file's line (the mtime of the truncation, which the record carries). If `sudo -n -u nobody` is refused (sudoers), use `sudo -n setpriv --reuid=65534 --regid=65534 --clear-groups truncate -s 4096 $F/file` instead (it drops every capability with the uid) and say so. A mode of `4777` after the truncation means the kernel did not clear the setuid bit, so the record carries no mode: stop and report.

## STEP 5: check the image

The log, with 2.4.4's zdb, on the exported pool (zdb opens it read-only):

```sh
D=/var/tmp/task-1121-sai; L=/var/log/task-1121-sai; Z=$D/zfs; M=$D/img
sudo -n $Z/zdb -e -p $M -ivv testpool_setattr/fs > $L/zdb-ivv.txt 2>&1; echo zdb-rc=$?
grep -E 'ZIL header|TX_[A-Z_]+ +len|foid|AT_' $L/zdb-ivv.txt
sudo -n $Z/zdb -l $M/setattr_size.dat | grep -E "^ +(name|state|txg|pool_guid|hostid|hostname|version):" | sort -u
```

Expected: rc 0; `ZIL header: claim_txg 0, claim_blk_seq 0, claim_lr_seq 0 replay_seq 0, flags 0x0`; exactly ONE record line, `TX_SETATTR len ..., txg ..., seq ...`, followed by `foid <n>, mask 0x2069` and `AT_GID <n>` (see "Note on zdb"). Quote the grep outputs whole. Stop and report if there is no `TX_SETATTR`, any other record type, more than one record, or a mask whose bits 0x1 and 0x8 are not both set (a mask other than 0x2069 with both set is reported, not a stop). The label lines are quoted as they are: the image will carry this VM's hostname and hostid in its labels, as the existing images of the test suite carry theirs; main decides.

Then a control: 2.4.4 replays a copy of the image, as an upgrade would after the crash. The original file is never imported.

```sh
D=/var/tmp/task-1121-sai; Z=$D/zfs; M=$D/img
sha256sum $M/setattr_size.dat
mkdir -p $M/check && cp $M/setattr_size.dat $M/check/
sudo -n $Z/zpool import -o cachefile=none -f -d $M/check -R $M/alt2 testpool_setattr; echo import-rc=$?
stat -c '%n %s %a %U:%G %Y %y' $M/alt2/testpool_setattr/fs/file
sudo -n $Z/zpool destroy testpool_setattr; echo destroy-rc=$?
rm -rf $M/check $M/alt2; sha256sum $M/setattr_size.dat
sudo -n $Z/zpool list -H -o name
```

Expected: import and destroy rc 0; the file `4096 777 root:root`, with the same `%y` as T4 to the nanosecond (the replay applied the record's mode and mtime); the two sha256 of the original identical; no pool left. Then every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|WARNING` (none expected).

## STEP 6: compress and hand over

```sh
D=/var/tmp/task-1121-sai; L=/var/log/task-1121-sai; M=$D/img
bzip2 -9 -c $M/setattr_size.dat > $L/setattr_size.dat.bz2
sha256sum $M/setattr_size.dat; bzcat $L/setattr_size.dat.bz2 | sha256sum
sha256sum $L/setattr_size.dat.bz2; stat -c %s $L/setattr_size.dat.bz2
base64 -w 76 $L/setattr_size.dat.bz2 > $L/setattr_size.dat.bz2.b64; wc -l -c < $L/setattr_size.dat.bz2.b64
```

The two sha256 of the uncompressed image must match. Expected size of the `.bz2`: a few tens of KiB (the empty 64 MiB pool image of the suite, `unclean_export.dat.bz2`, is 14 KiB). Above 96 KiB: stop and report the size, do not print the base64.

## STEP 7: end

`sudo -n $Z/scripts/zfs.sh -u` (quote rc); `lsmod | grep -E '^(zfs|spl) '` empty; `ls -l /etc/zfs/zpool.cache 2>&1` (absent expected; anything else is reported and left); `rm -rf $D`; `sudo -n fstrim -av`; `df -h /var/tmp`. Keep `$L` and everything in it: TASK-1121-SAT reads `$L/setattr_size.dat.bz2`.

## Report

Write `$L/report.md` and give the same text as your final message: hostname, kernel, srcversion and load line; the outputs of steps 4 and 5 (rc lines, the `stat` lines, T4, the zdb grep outputs, the label lines, the control's `stat` line and whether its mtime equals T4); "no VERIFY/ASSERT/PANIC/BUG/Oops/WARNING" or what was seen; the sha256 of `setattr_size.dat` and of `setattr_size.dat.bz2`, the size of the `.bz2` in bytes; then, last, the base64, between a line `-----BEGIN setattr_size.dat.bz2-----` and a line `-----END setattr_size.dat.bz2-----`, exactly as `$L/setattr_size.dat.bz2.b64` holds it. One-line verdict before the base64: "SAI image OK: one TX_SETATTR, mask 0x2069, 2.4.4 replays it to 4096 777 T4; bz2 <bytes> bytes sha256 <hash>" or what differs.
