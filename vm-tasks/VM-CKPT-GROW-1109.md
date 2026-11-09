# VM task: checkpoint discard after a DDT copy extension on a grown vdev

These instructions are for VM 106: Debian 13, kernel 6.12, any vCPU count.
STEP 0 stops on any other shape. Reply with what STEP 4 prints.

**This task may leave the VM with a hung pool.** The master arm (STEP 3) is
expected to fail a `VERIFY` in the sync thread. On Linux that stops the
thread without panicking the kernel: the pool and the zfs module then stay
stuck until a reboot. Do not reboot and do not try to unload anything after
STEP 3: report, and say the VM needs a reboot. Nothing else may run on this
VM until then.

**Background.** On master, a dedup entry that gets an extra copy after a
checkpoint keeps its old birth. If the vdev grew after the checkpoint, the
extra copy can land beyond the vdev's old size, and its free is sent to the
checkpoint space map, which still has the old size. In userspace (libzpool,
`ASSERT`s removed) this went through silently and the next checkpoint discard
failed on `VERIFY3U(entry_offset, <, sm->sm_start + sm->sm_size)` in
`space_map_incremental_destroy()`, on 5 runs out of 5, and never with the
fix of #19476. This task checks the same on the kernel module, release build.

**What is compared.** Two release builds of the kernel modules, one set of
tools:

| arm | commit | what it is |
|---|---|---|
| `F` | `609fbc43ca996c6014cd228e63c2b78859671475` | master + #19476 (`zio: avoid extending DDT phys across txgs`) + its tests |
| `M` | `47d0a322063639ed85aebce0ca8dc3af2605a75d` | upstream master, the commit #19476 sits on |

#19476 changes only `module/` and tests, so the tools of `M` serve both.
`F` runs first (expected to survive, twice), then `M` (expected to fail).

**Rules.**
- Work only under `/var/tmp/ckgrow` (W) and `/var/log/ckgrow` (L), and on
  the pool `ckgrow`. Never run `zpool`, `zfs` or `zdb` while none of this
  task's modules is loaded.
- Run each block as your usual user in bash; the blocks call `sudo -n`
  themselves where root is needed.
- If a check does not print what its comment says, stop and report; do not
  rebuild, repair or retry.
- Run STEP 2 and STEP 3 with your shell tool's background mode, one at a
  time, and wait for each to exit. If STEP 3 has not exited 5 minutes after
  it started, do not wait longer: go to STEP 4, which reads only files and
  the kernel log.

## STEP 0: which VM, and is it free

```sh
date -u '+%F %T'; hostname; id -un; sudo -n true && echo "sudo ok"; uname -r; nproc
grep -E '^(PRETTY_NAME|VERSION_ID)=' /etc/os-release
ls -d "/lib/modules/$(uname -r)/build"
for t in git make gcc dd truncate modinfo insmod rmmod udevadm; do PATH=/usr/sbin:/sbin:$PATH command -v $t > /dev/null || echo "missing $t"; done
grep -E '^(zfs|spl) ' /proc/modules                              # nothing
find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d 2>/dev/null  # nothing
pgrep -af 'zfs-tests|test-runner|ztest|zloop|configure|make|cc1|fio' | grep -v pgrep  # nothing
ls -d /var/tmp/ckgrow /var/log/ckgrow 2>&1        # No such file or directory, twice
df -h /var/tmp | tail -1                           # 25G free or more
```

Stop, changing nothing, if: `sudo -n true` fails; the system is not Debian
13 on a 6.12 kernel; the kernel headers or a tool are missing; a zfs or spl
module is loaded or a pool is imported; a build or test runs; W or L
exists; less than 25G is free on `/var/tmp` (the VMs keep 20G free).

## STEP 1: sources and builds (about 6 min)

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/ckgrow L=/var/log/ckgrow
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && mkdir -p $W/ko || exit 1
sudo -n dmesg | wc -l > $L/dmesg.start
git init -q $W/repo && git -C $W/repo fetch -q --depth 8 https://github.com/tigerblue77/zfs test-19476-on-47d0a32 || exit 1
git -C $W/repo rev-parse FETCH_HEAD                  # 609fbc43ca996c6014cd228e63c2b78859671475
git -C $W/repo worktree add -q --detach $W/tree 47d0a322063639ed85aebce0ca8dc3af2605a75d || exit 1
{
cd $W/tree || exit 1
j=$(nproc)
for t in M:47d0a322063639ed85aebce0ca8dc3af2605a75d F:609fbc43ca996c6014cd228e63c2b78859671475; do
	n=${t%%:*} c=${t#*:}
	git checkout -q --detach "$c" || break
	t0=$(date +%s)
	if [ "$n" = M ]; then
		nice -n 10 sh -c "./autogen.sh && ./configure --disable-debug --disable-debuginfo --with-config=all && make -j$j" > "$L/build-$n.log" 2>&1
	else
		nice -n 10 make -C module -j$j > "$L/build-$n.log" 2>&1
	fi
	rc=$?
	echo "$n at $(git rev-parse HEAD) build rc=$rc wall=$(( $(date +%s) - t0 ))s"
	[ "$rc" = 0 ] || break
	mkdir -p "$W/ko/$n" && cp module/spl.ko module/zfs.ko "$W/ko/$n/" || break
	echo "$n zfs.ko srcversion=$(modinfo -F srcversion "$W/ko/$n/zfs.ko") vermagic=$(modinfo -F vermagic "$W/ko/$n/zfs.ko" | cut -d' ' -f1)"
done
echo "release=$(grep -c '^KERNEL_DEBUG_CPPFLAGS = -UDEBUG -DNDEBUG' Makefile)"
echo "F recompiled zio.o: $(grep -cE 'CC \[M\] .*[ /]zio\.o$' "$L/build-F.log" 2>/dev/null)"
ls -l $W/tree/zpool $W/tree/zfs | wc -l                          # 2
} 2>&1 | tee $L/builds.txt
```

Expected: two `at <its commit> build rc=0` lines; two different
srcversions; `vermagic` = `uname -r` twice; `release=1`; `F recompiled
zio.o: 1`; `2`. On a failed build quote `grep -nE 'error:|Error [0-9]'
$L/build-<arm>.log | head -20` and stop.

## STEP 2: the script, then arm F twice (about 2 min)

```sh
W=/var/tmp/ckgrow L=/var/log/ckgrow
cat > $L/repro.sh <<'SCRIPT'
#!/bin/bash
# usage: repro.sh <arm> <run>; as root
export PATH=/var/tmp/ckgrow/tree:/usr/sbin:/sbin:$PATH
W=/var/tmp/ckgrow L=/var/log/ckgrow a=$1 r=$2 D=$W/ko/$1 d=$W/pool p=ckgrow
say() { echo "$(date -u +%T) $a#$r $*"; }
newk() { dmesg | tail -n +$(( $(cat $L/dmesg.start) + 1 )) | grep -E 'VERIFY|PANIC|ASSERT|BUG:|Oops|blocked for more'; }
[ -z "$(find /proc/spl/kstat/zfs -mindepth 1 -maxdepth 1 -type d 2> /dev/null)" ] || { say "a pool is imported: stopping"; exit 1; }
for m in zfs spl; do [ -d /sys/module/$m ] && { rmmod $m || { say "rmmod $m failed"; exit 1; }; }; done
modprobe -q zlib_deflate; modprobe -q zlib_inflate
insmod $D/spl.ko && insmod $D/zfs.ko || { say "load failed"; exit 1; }
udevadm settle
for m in spl zfs; do
	[ "$(cat /sys/module/$m/srcversion)" = "$(modinfo -F srcversion $D/$m.ko)" ] || { say "$m is not arm $a's"; exit 1; }
done
say "loaded zfs $(cat /sys/module/zfs/srcversion)"
rm -rf $d && mkdir -p $d || exit 1
truncate -s 384M $d/vdev || exit 1
zpool create -o cachefile=none -o feature@block_cloning=disabled \
    -O dedup=sha256 -O compression=off -O recordsize=128k \
    -O mountpoint=$d/mnt $p $d/vdev || exit 1
zfs create -o dedup=off $p/r && zfs create -o copies=1 $p/a &&
    zfs create -o copies=2 $p/b || exit 1
dd if=/dev/urandom of=$d/mnt/r/reserve bs=1M count=8 status=none
dd if=/dev/urandom of=$d/mnt/r/early bs=1M count=4 status=none
dd if=/dev/urandom of=$d/mnt/a/f bs=128k status=none 2>/dev/null
say "filled: $(stat -c %s $d/mnt/a/f) bytes in a/f"
zpool sync $p
rm $d/mnt/r/reserve && zpool sync $p
zpool checkpoint $p || { say "checkpoint failed"; exit 1; }
rm $d/mnt/r/early && zpool sync $p
truncate -s 896M $d/vdev && zpool online -e $p $d/vdev || { say "expand failed"; exit 1; }
say "size after expand: $(zpool get -Hp -o value size $p)"
dd if=$d/mnt/a/f of=$d/mnt/b/f bs=128k status=none || { say "copy failed"; exit 1; }
zpool sync $p
rm $d/mnt/a/f $d/mnt/b/f
zpool sync $p && zpool sync $p
say "checkpointed space before discard: $(zpool get -Hp -o value checkpoint $p)"
[ -z "$(newk)" ] || { newk; say "RESULT kernel message before the discard"; exit 3; }
zpool checkpoint -d $p || { say "discard request failed"; exit 1; }
for i in $(seq 1 90); do
	sleep 2
	if [ -n "$(newk)" ]; then newk; say "RESULT VERIFY during the discard"; exit 3; fi
	if [ "$(zpool get -H -o value checkpoint $p)" = "-" ]; then break; fi
done
[ "$(zpool get -H -o value checkpoint $p)" = "-" ] || { say "RESULT discard not finished after 180 s, no kernel message"; exit 4; }
say "RESULT discard completed"
zpool destroy $p && rm -rf $d && rmmod zfs && rmmod spl && say "cleaned"
SCRIPT
chmod +x $L/repro.sh
for r in 1 2; do sudo -n $L/repro.sh F $r 2>&1 | tee -a $L/run-F.log; done
```

Expected, twice: `RESULT discard completed`, then `cleaned`. Anything
else: stop, go to STEP 4 with what you have, and do not run STEP 3.

## STEP 3: arm M (about 1 min, or a hang)

```sh
sudo -n /var/log/ckgrow/repro.sh M 1 2>&1 | tee /var/log/ckgrow/run-M.log
```

Expected: `RESULT VERIFY during the discard` (exit 3), the `VERIFY` line
of `space_map_incremental_destroy()` printed above it. The pool then stays
hung: leave it. If the command has not exited after 5 minutes, go to STEP 4.

## STEP 4: report

```sh
L=/var/log/ckgrow
cat $L/builds.txt; echo ==; cat $L/run-F.log; echo ==; cat $L/run-M.log 2>/dev/null
echo "== kernel messages since STEP 1"
sudo -n dmesg | tail -n +$(( $(cat $L/dmesg.start) + 1 )) | grep -vE 'loop[0-9]|audit' | head -60
echo "== hung threads"; ps -eo pid,stat,wchan:32,comm | awk '$2 ~ /D/' | head
df -h /var/tmp | tail -1
```

Reply with all of it. If STEP 3 ended in a hang or a `VERIFY`, add: "VM 106
needs a reboot; pool ckgrow and the zfs module are stuck." Do not run
`fstrim` while a pool is hung; if STEP 3 did not run or ended with
`cleaned`, run `sudo -n rm -rf /var/tmp/ckgrow && sudo -n fstrim -av` and
add its summary.
