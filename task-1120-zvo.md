TASK-1120-ZVO (from the coordinator session "main"): functional, about 45 min, debug build only (one tree), no reboot. Any of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB).

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters. The scripts of step 2 contain all three.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1120-zvo`, `L=/var/log/task-1120-zvo`.

Never run anything outside the steps of this task, and never import in the kernel a pool a test or a command left behind: record `ls -l` of its files, then remove them. If a result surprises you, stop and report it with the outputs.

## Why

On Linux 5.10 and older (AlmaLinux 8's 4.18 included), `zvol_open()` can use a `zvol_state_t` that `zvol_remove_minors_impl()` has already freed. It reads the zvol from `disk->private_data` and then takes `zv_state_lock`, with nothing holding the zvol in between, and `zvol_os_remove_minor()` relies on `del_gendisk()` to wait for such an opener once `private_data` is NULL. That holds on 5.14 and newer, where `fops->open()` runs under `disk->open_mutex` and `del_gendisk()` takes it, but not on 5.10 and older, where it runs under the whole disk's `bd_mutex`, which `del_gendisk()` never takes. A fork CI run on AlmaLinux 8 (4.18.0-553) panicked this way in `zvol_misc_volmode_race` (`list_del corruption` in `__mutex_remove_waiter()` from `zvol_open()`, systemd-udevd).

The fix `e256741ff14` ("Linux: wait for zvol_open() before freeing a removed zvol") adds a configure check for `bdget_disk()` and `bd_mutex`, and where they exist takes and drops the whole disk's `bd_mutex` right after `del_gendisk()`, before the zvol can be freed.

**This VM cannot reproduce the bug, and the fix compiles out here.** On 6.12, `del_gendisk()` takes `disk->open_mutex`, under which every `zvol_open()` and `zvol_release()` runs, so the window is closed; and 6.12 has neither `bdget_disk()` nor `bd_mutex`, so the configure check says `no` and the new code is not built. What this task checks: that the check says `no` and the module builds and loads; and that the race amplifier (step 2), which is meant for a 5.10 or older kernel later, does what it should on a kernel where it must stay harmless: many opens of the zvol overlap its removal (ENXIO counted), the minor is replaced many times, and nothing appears in dmesg.

What is tested: fork https://github.com/tigerblue77/zfs, branch `zvol-open-pin-state`, head `e256741ff1402f5bd52753c7a12acdd33c525473`, tree `f6219786d7050f09117ab9a7497959e971590c0d`, one commit on upstream master `1b1aa0e91b6`.

The parts:

- `race.sh <tag> <seconds>` (step 2, run as root): a pool `zvorace` on a 1 GiB file under $D, a 64 MiB volume `zvorace/vol` with `volmode=full`. For `<seconds>`, one loop switches `volmode` between `dev` and `full` (each switch removes the minor and creates it again, as `zvol_misc_volmode_race` does), while 8 openers (`opener.py`, each in its own systemd scope with `CPUQuota=2%`, so that an opener is often descheduled in the middle of an open) open and close the zvol's `/dev/zdN` in a loop. It then destroys the pool, removes the file, and prints one line: `<tag> dev=<dev> churn=<switch pairs> opens=<n> enxio=<n> enoent=<n> other=<n> destroy_rc=<rc> dmesg_findings=<n>`.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- This task creates only the pool `zvorace` (the script creates and destroys it) on a file under $D; touch no block device of the VM. Never run `zpool import` yourself; never `zfs-tests.sh -x`. A pool this task did not create (`tank` or any other) is a stop: report it, and leave it as it is (no export, no destroy, no import).
- Before every `race.sh`: `lsmod | grep -E '^zfs '` shows the module of this task only, `timeout 30 sudo -n $D/zt/zpool list -H -o name` shows no pool, `ls /sys/block | grep -E '^zd'` shows nothing, `sudo -n losetup -a` shows nothing, and `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK.
- Every build and every `race.sh` runs detached (`setsid nohup ... &`) and is polled. A hang is: a build still going after 90 minutes; a `race.sh` of 600 s still going after 20 minutes. Then capture `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zfs|zpool|python3|systemd-udevd'`, the kernel stacks of every D task (`sudo -n cat /proc/<pid>/stack`) and `sudo -n dmesg | tail -n 150` into $L/hang.txt, then stop and report.
- Reboots: this task pre-approves none; for this task that replaces the standing approval of 18/11. A hang, a panic or a module that will not unload: capture as above, stop, report, and wait for main.
- `sudo -n fstrim -av` after every deletion of trees or vdev files. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 20 GiB free on /var/tmp), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sudo -n losetup -a` (empty), `ls -d $D $L 2>&1` (both absent), `id zts`, `systemd-run --version | head -n 1`, `python3 --version`, and `pgrep -a -x zed; pgrep -a -x lt-zed` (must be empty: a running zed is a stop; do not start or stop zed).

Then `ls -l /etc/zfs/zpool.cache 2>&1`: absent is expected. If it exists with size 0, move it aside and say so:

```sh
L=/var/log/task-1120-zvo
sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L && sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.step0
```

If it exists with a size above 0, stop and report its `ls -l`: do not read it with zdb, do not load the module.

Trees and logs other tasks keep under /var/tmp and /var/log stay untouched. Any other difference: stop and report.

## STEP 1: fetch and check

```sh
D=/var/tmp/task-1120-zvo; L=/var/log/task-1120-zvo
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && chmod 755 $D
mkdir $D/zt && cd $D/zt && git init -q . && git fetch -q --depth 2 https://github.com/tigerblue77/zfs zvol-open-pin-state && git checkout -q FETCH_HEAD
echo "$(git rev-parse HEAD) $(git rev-parse HEAD~1) $(git write-tree) $(git status --porcelain | wc -l)" > $L/tree.txt
echo "e256741ff1402f5bd52753c7a12acdd33c525473 1b1aa0e91b694798085a2771c4eef40e93b9b2db f6219786d7050f09117ab9a7497959e971590c0d 0" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREE-OK
git diff --stat=80 HEAD~1 HEAD; grep -c 'HAVE_BLKDEV_BD_MUTEX' module/os/linux/zfs/zvol_os.c config/kernel-blkdev.m4
```

Expected TREE-OK, the diffstat

```
 config/kernel-blkdev.m4       | 31 +++++++++++++++++++++++++++++++
 module/os/linux/zfs/zvol_os.c | 25 +++++++++++++++++++++++--
 2 files changed, 54 insertions(+), 2 deletions(-)
```

and `module/os/linux/zfs/zvol_os.c:2`, `config/kernel-blkdev.m4:1`. If the fetch fails, wait 1 minute and try once more. Any other difference (the branch may have moved): stop and report as is.

## STEP 2: the two scripts

Write each script below, byte for byte (4-space indentation, no tab, a newline after the last line), with `cat > <file> <<'EOF'` ... `EOF` (the quoted delimiter keeps everything literal): the first to $L/race.sh, the second to $L/opener.py. Then:

```sh
L=/var/log/task-1120-zvo
printf '%s\n' "209dabe9c7dbb7097036654e812974b9dce395f27f1b50b3f9651ff521f292dd  $L/race.sh" "2c7fddcb59a6a08dc2072c40db4affc34122b0c40931878dfdbcf043489a3e79  $L/opener.py" | sha256sum -c && sh -n $L/race.sh && python3 -c "import ast, sys; ast.parse(open(sys.argv[1]).read())" $L/opener.py && echo SCRIPTS-OK
```

Expected two `OK` lines and SCRIPTS-OK. If a sum differs, write that file again from this text (an HTML entity left in it is the usual cause); a second mismatch: stop and report.

```sh
#!/bin/sh
# TASK-1120-ZVO open/remove race amplifier, run as root: race.sh <tag> <seconds>
D=/var/tmp/task-1120-zvo
L=/var/log/task-1120-zvo
Z=$D/zt
tag=$1
secs=$2
p=zvorace
v=$D/vdev-$tag
truncate -s 1G $v || exit 1
$Z/zpool create -o cachefile=none -O mountpoint=none $p $v || exit 1
$Z/zfs create -V 64M -o volmode=full $p/vol || exit 1
udevadm settle
zds=$(ls /sys/block | grep -E '^zd[0-9]+$')
if [ "$(echo $zds | wc -w)" -ne 1 ]; then
    echo "$tag STOP zd devices: [$zds]" | tee -a $L/race.txt
    $Z/zpool destroy $p
    rm -f $v
    exit 1
fi
dev=/dev/$zds
n0=$(dmesg | wc -l)
end=$(( $(date +%s) + secs ))
(
    n=0
    while [ "$(date +%s)" -lt $end ]; do
        $Z/zfs set volmode=dev $p/vol
        $Z/zfs set volmode=full $p/vol
        n=$((n + 1))
    done
    echo "churn=$n"
) > $L/churn-$tag.txt 2>&1 &
cpid=$!
i=1
while [ $i -le 8 ]; do
    systemd-run --quiet --scope --unit=zvorace-$tag-$i \
        -p CPUQuota=2% \
        python3 $L/opener.py $dev $end > $L/open-$tag-$i.txt 2>&1 &
    i=$((i + 1))
done
wait $cpid
wait
$Z/zpool destroy $p
drc=$?
rm -f $v
bad=$(dmesg | tail -n +$((n0 + 1)) |
    grep -cE 'list_del|BUG|Oops|general protection|KASAN|use-after-free|VERIFY|PANIC|blocked for more than')
echo "$tag dev=$dev $(cat $L/churn-$tag.txt) $(cat $L/open-$tag-*.txt | awk '{ for (f = 1; f <= NF; f++) { split($f, kv, "="); s[kv[1]] += kv[2] } } END { printf "opens=%d enxio=%d enoent=%d other=%d", s["ok"], s["enxio"], s["enoent"], s["other"] }') destroy_rc=$drc dmesg_findings=$bad" | tee -a $L/race.txt
```

```python
# TASK-1120-ZVO opener: open and close <dev> in a loop until <end> (epoch s)
import errno
import os
import sys
import time

dev = sys.argv[1]
end = float(sys.argv[2])
n = {"ok": 0, "enxio": 0, "enoent": 0, "other": 0}
while time.time() < end:
    try:
        os.close(os.open(dev, os.O_RDONLY))
        n["ok"] += 1
    except OSError as e:
        if e.errno == errno.ENXIO:
            n["enxio"] += 1
        elif e.errno == errno.ENOENT:
            n["enoent"] += 1
        else:
            n["other"] += 1
print(" ".join("%s=%d" % kv for kv in n.items()))
```

## STEP 3: debug build (about 12 minutes)

```sh
D=/var/tmp/task-1120-zvo; L=/var/log/task-1120-zvo
setsid nohup sh -c "cd $D/zt && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build.log`. It must end `build-rc=0`; quote every `warning:` line outside objtool notes, and quote `grep -E 'bdget_disk\(\) and bd_mutex' $L/build.log` (expected `checking whether bdget_disk() and bd_mutex are available... no`) and `grep -c HAVE_BLKDEV_BD_MUTEX $D/zt/zfs_config.h` (expected `1`, the commented-out `#undef` line: `grep HAVE_BLKDEV_BD_MUTEX $D/zt/zfs_config.h` shows `/* #undef HAVE_BLKDEV_BD_MUTEX */`). A `yes` there is a surprise: stop and report. Note S1=`modinfo -F srcversion $D/zt/module/zfs.ko`.

## STEP 4: the amplifier (tags a1, a2)

`sudo -n $D/zt/scripts/zfs.sh`; quote `cat /sys/module/zfs/srcversion` (must equal S1) and the last `ZFS: Loaded module` line ("(DEBUG mode)"). Note N0=`sudo -n dmesg | wc -l`. Run the pre-run checks of the VM rules, then a 60 s run first (tag a1), and if its line shows `destroy_rc=0 dmesg_findings=0`, a 600 s run (tag a2) after the pre-run checks:

```sh
L=/var/log/task-1120-zvo
setsid nohup sudo -n sh $L/race.sh a1 60 > $L/race-a1.nohup 2>&1 < /dev/null &
```

```sh
L=/var/log/task-1120-zvo
setsid nohup sudo -n sh $L/race.sh a2 600 > $L/race-a2.nohup 2>&1 < /dev/null &
```

Poll `cat $L/race.txt` until the tag's line appears. For each run quote its line, `cat $L/race-<tag>.nohup` and `cat $L/open-<tag>-1.txt`. Expected: `destroy_rc=0`, `dmesg_findings=0`, `churn` above 0, `opens` above 0, `other=0`. `enxio` and `enoent` count opens that met the removal: report them as they are (no threshold; both 0 in a2 would mean the amplifier does not overlap opens with removals, say so). A `STOP` line, `other` above 0, or a non-empty nohup file: quote it and stop. While a2 runs, sample once, about 5 minutes in, `systemctl list-units --type=scope 'zvorace-*' --no-legend | wc -l` (expected 8) and `cat /sys/fs/cgroup/system.slice/zvorace-a2-1.scope/cpu.max 2>&1; cat /sys/fs/cgroup/zvorace-a2-1.scope/cpu.max 2>&1` (one of the two is expected to print `2000 100000`; report which path worked).

After them: the pre-run checks once more, and every line of `sudo -n dmesg | tail -n +$((N0+1))` matching `list_del|VERIFY|ASSERT|PANIC|BUG|Oops|blocked for more than|segfault|general protection` quoted with 10 lines before it (none expected). Unload with `sudo -n $D/zt/scripts/zfs.sh -u`; `lsmod | grep -E '^(zfs|spl) '` must be empty.

## STEP 5: cleanup

`lsmod | grep -E '^(zfs|spl) '` (empty), `sudo -n losetup -a` (empty), `ls /sys/block | grep -E '^zd'` (empty), `sudo -n rm -rf $D`, `sudo -n fstrim -av`, `df -h /var/tmp`. Then `ls -l /etc/zfs/zpool.cache 2>&1`: if it exists with size 0, `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; with a size above 0, report it and leave it. Logs and scripts stay in $L.

## Report

Write $L/report.md and give the same text as your final message:

1. Environment: VM, kernel, Debian version, nproc, RAM, systemd and python versions, the zed line of step 0; TREE-OK, diffstat and the grep counts; SCRIPTS-OK; build rc and warnings; the configure line for `bdget_disk() and bd_mutex` and the `zfs_config.h` line; S1 and DEBUG mode; what happened to /etc/zfs/zpool.cache at steps 0 and 5.
2. Table: tag | seconds | churn | opens | enxio | enoent | other | destroy_rc | dmesg_findings. Then the scope count and `cpu.max` sample of a2. Then one verdict line, e.g. "fix e256741ff14 on 6.12: check says no (fix compiled out, as expected); amplifier: N volmode switch pairs, M opens of which K ENXIO in 600 s, no dmesg finding", or what differs.
3. Quotes: each run's line, nohup files, `open-<tag>-1.txt`, the dmesg check.
4. Deviations, cleanup, then End.
