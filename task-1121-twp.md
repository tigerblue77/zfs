TASK-1121-TWP (from the coordinator session "main"): functional, about 1 h 15 builds included, one reboot. M-43: our fix for a RAID-Z expansion that an earlier release accepted on a vdev too wide for the scratch area. (A) Fail-before: master's module imports such a pool read-write: VERIFY in txg_sync, the pool wedges, one reboot. (B) With the fix: the import parks the expansion and the pool stays usable. Any of the three test VMs (Debian 13, kernel 6.12, 8 vCPU, 7.8 GB). Other VMs may run functional tasks at the same time: this task measures no performance.

End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters. The program of step 2 contains all three.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs. In this task `D=/var/tmp/task-1121-twp`, `L=/var/log/task-1121-twp`. `$D/m` is the fail-before tree (master plus a test-only patch), `$D/f` the fix.

Never run anything outside the steps of this task. If a result surprises you, stop and report it with the outputs.

## Why

Before 7c546ef8e5a ("raidz: refuse expansions that the boot area cannot hold", #19365), `vdev_raidz_attach_check()` refused nothing, so every release up to 2.4.x accepts `zpool attach` of a 57th child to a RAID-Z vdev at ashift 16 (a 113th at ashift 15, and so on). The attach is committed, then `raidz_reflow_scratch_sync()` fails `VERIFY3U(write_size, >=, raidvd->vdev_children << ashift)` (3670016 >= 3735552) in the syncing thread. #19365 refuses new attaches of this kind, but a pool that already holds one fails the same VERIFY on every read-write import, with master too, because the expansion thread starts again with the scratch phase: on Linux txg_sync stops and the pool hangs.

The fix `cdccdbee036` ("raidz: stop an expansion too wide for the scratch area") checks the width in `spa_raidz_expand_thread()` before the scratch phase and parks the expansion: one console warning `WARNING: pool 'tw': raidz vdev 0 is too wide to be expanded to 57 children, its expansion is stopped` per import, a `reflow too wide` record in the pool history (which also posts a history_event and a zfs_dbgmsg line), and an in-memory flag that keeps the thread from running again. Nothing has been reflowed (offset 0), so the pool stays usable read-write in the old layout with the limits of any expansion in progress: no other attach, no checkpoint; `zpool status` shows the expansion at 0.00% with no estimate, and `zpool wait -t raidz_expand` does not return. Nothing changes on disk.

Main reproduced both sides in user space (libzpool) on 21/11. This task runs the kernel path on Linux 6.12.

How the pool is made. Master refuses such an attach, so the state is made in user space with a test-only bypass: the patch of step 1 (never committed, never pushed) applied to master makes `vdev_raidz_attach_check()` accept the attach when the environment variable `TWP_OLD_ATTACH` is set, as releases before 7c546ef8e5a did. It sits under `#ifndef _KERNEL`, so the kernel module built from that tree behaves exactly as master's. The program `twp_create` (step 2), linked against that tree's libzpool and run as your unprivileged login user, creates pool `tw` as one raidz1 of 56 sparse 256 MiB files at ashift 16 in `$D/vdev` and attaches a 57th; libzpool's syncing thread then fails the VERIFY and the process aborts (exit 134, expected), leaving the pool never exported, with the expansion recorded and nothing reflowed: what a host that hung or panicked on the attach would leave. No kernel module is loaded at that point.

What is tested: fork https://github.com/tigerblue77/zfs, branch `raidz-expand-too-wide-park`, head `cdccdbee0365e108a42ec0f4f71a93573e9d66e8`, tree `81e1b01ab63ca8643f563874e81517015332245f`, one commit on upstream master `a1daebfec3f5ca46ac261ef339efdf0dbeae997c` (20/11, tree `0b23c141b417a8542546e1f7a32662bc82bc0f92`). The fail-before tree is that master plus the patch: tree `812b2dd2726601c42e8eed1a8f66b60ab5f04a49`.

## VM rules

- Nothing is published on GitHub (fetching is fine). Never display, copy or create a credential file.
- This task creates only the pool `tw`, on files under `$D/vdev`; touch no block device of the VM and no other pool. Every `zpool import` here names `-d $D/vdev tw`; never `zpool import -a`; no ZTS.
- Every import of `tw` uses `-o cachefile=none` (nothing is written to /etc/zfs/zpool.cache) and `-N`: the root dataset was created by libzpool, which puts no ZPL in it, so it cannot be mounted; the file system of step 6 is a child.
- Before each module load (steps 4 and 6): `lsmod | grep -E '^(zfs|spl) '` empty, `ls -l /dev/zfs 2>&1` absent, `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` prints CACHE-OK.
- Builds run detached (`setsid nohup ... &`) and are polled; a build still going after 90 minutes is a hang.
- Part A (step 4) wedges the pool on purpose. From the moment its import starts until the reboot, run no zpool, zfs, zdb or `scripts/zfs.sh` command, no plain `sync` and no `df`: they would sleep uninterruptibly and your tool call would hang. Use only dmesg, ps and /proc.
- Reboots: exactly one is pre-approved, in step 5, after part A. Before it, arrange your own resumption the way DBM3 and DBM4 did (a message scheduled to your own session about 5 minutes later), say which, and write in $L/progress.md where you are. A global `sync` may block once txg_sync is wedged: use `sync -f $L/progress.md`, then the sysrq reboot through a transient unit: `sudo -n systemd-run --on-active=15 /bin/sh -c 'echo b > /proc/sysrq-trigger'`.
- Part B (step 6): its imports run detached and are polled. Once an import has returned with rc 0 and dmesg shows no VERIFY, ASSERT or PANIC, the other pool commands run in the foreground under the `timeout` given. A hang in part B (an import not returned after 120 s, a command that outlives its timeout), or a VERIFY, ASSERT or PANIC there: capture as in step 4 into $L/hang-b.txt, stop, report, and wait for main. No second reboot is pre-approved.
- `sudo -n fstrim -av` after deleting the trees and the vdev files. Modules unloaded at the end.

## STEP 0: state

`hostname`, `uname -r`, `uptime -s`, `nproc`, `free -m`, `df -h / /var/tmp` (at least 25 GiB free on /var/tmp: two trees and the vdev files), `lsmod | grep -E '^(zfs|spl) '` (must be empty), `ls -l /dev/zfs 2>&1` (must not exist), `sysctl kernel.panic_on_oops kernel.panic kernel.hung_task_panic kernel.hung_task_timeout_secs` (quote; if hung_task_panic is 1, set it to 0 for this task and say so), `ulimit -c` and `cat /proc/sys/kernel/core_pattern` (quote; change neither), `ls -l /etc/hostid 2>&1` (record), `ls -d $D $L 2>&1` (both absent), `pgrep -a -x zed; pgrep -a -x lt-zed` (must be empty: a running zed is a stop; do not start or stop one).

Then create $L (`sudo -n mkdir -p $L && sudo -n chown "$(id -un):" $L`) and look at `ls -l /etc/zfs/zpool.cache 2>&1`. Absent is expected. Size 0: move it to $L/zpool.cache.step0 and say so. Above 0: VM 107 may still hold the cache file that TASK-1121-DBM4's panicked run left (2364 bytes): if `ls -l` shows that size and `strings /etc/zfs/zpool.cache | grep -c testpool` is at least 1, move it to $L/zpool.cache.dbm4 and say so. Any other non-empty cache file: stop and report its `ls -l` (do not read it with zdb, do not load a module).

## STEP 1: trees and the test-only patch

```sh
D=/var/tmp/task-1121-twp; L=/var/log/task-1121-twp
sudo -n mkdir -p $D && sudo -n chown "$(id -un):" $D && chmod 755 $D
for t in m f; do mkdir $D/$t && git -C $D/$t init -q . && git -C $D/$t fetch -q --depth 2 https://github.com/tigerblue77/zfs raidz-expand-too-wide-park || echo "FETCH-FAILED $t"; done
git -C $D/f checkout -q FETCH_HEAD && git -C $D/m checkout -q FETCH_HEAD~1
echo "$(git -C $D/f rev-parse HEAD) $(git -C $D/f rev-parse HEAD^{tree}) $(git -C $D/m rev-parse HEAD) $(git -C $D/m rev-parse HEAD^{tree})" > $L/tree.txt
echo "cdccdbee0365e108a42ec0f4f71a93573e9d66e8 81e1b01ab63ca8643f563874e81517015332245f a1daebfec3f5ca46ac261ef339efdf0dbeae997c 0b23c141b417a8542546e1f7a32662bc82bc0f92" > $L/tree.expected
diff $L/tree.expected $L/tree.txt && echo TREES-OK
git -C $D/f log --oneline -2; git -C $D/f diff --stat=80 HEAD~1 HEAD
```

Expected TREES-OK, the subjects `cdccdbee036 raidz: stop an expansion too wide for the scratch area` and `a1daebfec3f ZTS: chattr_003_neg: accept uutils ln's wording for EPERM`, and the diffstat

```
 include/sys/vdev_raidz.h |  7 +++++++
 module/zfs/vdev_raidz.c  | 29 ++++++++++++++++++++++++++++-
 2 files changed, 35 insertions(+), 1 deletion(-)
```

If a fetch fails, wait 1 minute and try once more. Any other difference (the branch may have moved): stop and report as is.

The patch, written from base64 because it holds tabs:

```sh
D=/var/tmp/task-1121-twp; L=/var/log/task-1121-twp
base64 -d > $L/twp-bypass.patch <<'EOF_B64'
ZGlmZiAtLWdpdCBhL21vZHVsZS96ZnMvdmRldl9yYWlkei5jIGIvbW9kdWxlL3pmcy92ZGV2X3Jh
aWR6LmMKaW5kZXggY2QzZmUwMWFlNTkuLjQ1YmJiYzcyNGY0IDEwMDY0NAotLS0gYS9tb2R1bGUv
emZzL3ZkZXZfcmFpZHouYworKysgYi9tb2R1bGUvemZzL3ZkZXZfcmFpZHouYwpAQCAtNTQyNSw2
ICs1NDI1LDE1IEBAIHZkZXZfcmFpZHpfYXR0YWNoX2NoZWNrKHZkZXZfdCAqcmFpZHZkKQogCSAq
ID4yMDAgY2hpbGRyZW4pLgogCSAqLwogCWlmIChuZXdfY2hpbGRyZW4gPDwgcmFpZHZkLT52ZGV2
X2FzaGlmdCA+IFZERVZfQk9PVF9TSVpFKSB7CisjaWZuZGVmIF9LRVJORUwKKwkJLyoKKwkJICog
VEVTVCBPTkxZIChUQVNLLTExMjEtVFdQKSwgbmV2ZXIgcHVzaGVkOiB3aXRoIFRXUF9PTERfQVRU
QUNICisJCSAqIHNldCwgYSBsaWJ6cG9vbCBjb25zdW1lciBhY2NlcHRzIHRoZSBhdHRhY2ggYXMg
cmVsZWFzZXMKKwkJICogYmVmb3JlIDdjNTQ2ZWY4ZTVhIGRpZC4gIFRoZSBrZXJuZWwgbW9kdWxl
IGlzIHVuY2hhbmdlZC4KKwkJICovCisJCWlmIChnZXRlbnYoIlRXUF9PTERfQVRUQUNIIikgIT0g
TlVMTCkKKwkJCXJldHVybiAoMCk7CisjZW5kaWYKIAkJcmV0dXJuIChTRVRfRVJST1IoWkZTX0VS
Ul9SQUlEWl9FWFBBTkRfVE9PX1dJREUpKTsKIAl9CiAJcmV0dXJuICgwKTsK
EOF_B64
echo "414fb8b3df642bb9d0f9c808ed6a8bface994734c0968e62e2431259106e4a43  $L/twp-bypass.patch" | sha256sum -c
cd $D/m && git apply $L/twp-bypass.patch && git add -A && git write-tree && git reset -q && git status --porcelain
sha256sum $D/m/module/zfs/vdev_raidz.c $D/f/module/zfs/vdev_raidz.c $D/f/include/sys/vdev_raidz.h
grep -c TWP_OLD_ATTACH $D/m/module/zfs/vdev_raidz.c; grep -c vre_too_wide $D/f/module/zfs/vdev_raidz.c $D/f/include/sys/vdev_raidz.h
```

Expected: `OK`; the tree `812b2dd2726601c42e8eed1a8f66b60ab5f04a49` and ` M module/zfs/vdev_raidz.c`;

```
605c1072b402bf2a87706ec8b37b9bc9dc12885b56a503ae5aa1f1d0101cd50d  /var/tmp/task-1121-twp/m/module/zfs/vdev_raidz.c
b73896a00a3271e73d78321894ab7862d9896ade19d9d580902301449a3b8538  /var/tmp/task-1121-twp/f/module/zfs/vdev_raidz.c
5e8d85f89fd4df26bfcbbc73853c6f4037e8aaa84ac0f6cc03952f35c50742c9  /var/tmp/task-1121-twp/f/include/sys/vdev_raidz.h
```

then `2`, and `module/zfs/vdev_raidz.c:3`, `include/sys/vdev_raidz.h:1`. If the patch's sum differs, write it again (an HTML entity left in it is the usual cause); a second mismatch, or anything else different: stop and report. Do not edit any file of the trees by hand. For reading only, the patch is:

```
--- a/module/zfs/vdev_raidz.c
+++ b/module/zfs/vdev_raidz.c
@@ -5425,6 +5425,15 @@ vdev_raidz_attach_check(vdev_t *raidvd)
 	 * >200 children).
 	 */
 	if (new_children << raidvd->vdev_ashift > VDEV_BOOT_SIZE) {
+#ifndef _KERNEL
+		/*
+		 * TEST ONLY (TASK-1121-TWP), never pushed: with TWP_OLD_ATTACH
+		 * set, a libzpool consumer accepts the attach as releases
+		 * before 7c546ef8e5a did.  The kernel module is unchanged.
+		 */
+		if (getenv("TWP_OLD_ATTACH") != NULL)
+			return (0);
+#endif
 		return (SET_ERROR(ZFS_ERR_RAIDZ_EXPAND_TOO_WIDE));
 	}
 	return (0);
```

## STEP 2: builds (both at once, about 25 minutes) and the program

```sh
D=/var/tmp/task-1121-twp; L=/var/log/task-1121-twp
setsid nohup sh -c "cd $D/m && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build-m.log 2>&1 < /dev/null &
setsid nohup sh -c "cd $D/f && ./autogen.sh && ./configure --enable-debug && nice make -j4; echo build-rc=\$?" > $L/build-f.log 2>&1 < /dev/null &
```

Poll `grep build-rc= $L/build-*.log`. Each must end `build-rc=0`; quote every `warning:` line outside objtool notes (none expected: main built both trees in user space with `--enable-debug` without one). Note SM and SF = `modinfo -F srcversion <tree>/module/zfs.ko` for m and f; they must differ.

While they build, write the program below to $L/twp_create.c, byte for byte (4-space indentation, no tab, a newline after the last line), with `cat > $L/twp_create.c <<'EOF'` ... `EOF` (the quoted delimiter keeps everything literal), then check it:

```sh
L=/var/log/task-1121-twp
echo "84727719f4753d8d5f34376fc493e32d9007ee0e16d0134696a0cda1d636d938  $L/twp_create.c" | sha256sum -c
```

Expected `OK`. If it differs, write it again from this text; a second mismatch: stop and report.

```c
// SPDX-License-Identifier: CDDL-1.0
/*
 * TASK-1121-TWP: twp_create DIR
 * Creates pool "tw" in DIR as one raidz1 of 56 file vdevs (d00..d55,
 * sparse, 256 MiB) at ashift 16, then attaches d56. Built against a
 * libzpool with the test-only TWP_OLD_ATTACH bypass and run with
 * TWP_OLD_ATTACH=1, the attach is accepted, as releases before 7c546ef8e5a
 * did, and the next txg sync fails VERIFY3U(write_size, >=, ...) in
 * raidz_reflow_scratch_sync(): the process aborts and leaves the pool in
 * DIR, never exported, with the expansion recorded and nothing reflowed.
 * Run it as an unprivileged user; it touches nothing outside DIR.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_impl.h>
#include <sys/fs/zfs.h>
#include <zfeature_common.h>
#include <libzpool.h>

#define POOL    "tw"
#define WIDTH   56
#define ASHIFT  16
#define DEVSIZE (256ULL << 20)

static nvlist_t *
file_vdev(const char *dir, int i)
{
    char path[MAXPATHLEN];
    nvlist_t *nv = fnvlist_alloc();
    int fd;

    (void) snprintf(path, sizeof (path), "%s/d%02d", dir, i);
    fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
    VERIFY3S(fd, >=, 0);
    VERIFY0(ftruncate(fd, DEVSIZE));
    (void) close(fd);

    fnvlist_add_string(nv, ZPOOL_CONFIG_TYPE, VDEV_TYPE_FILE);
    fnvlist_add_string(nv, ZPOOL_CONFIG_PATH, path);
    fnvlist_add_uint64(nv, ZPOOL_CONFIG_ASHIFT, ASHIFT);
    return (nv);
}

static nvlist_t *
root_vdev(nvlist_t **child, int n)
{
    nvlist_t *root = fnvlist_alloc();

    fnvlist_add_string(root, ZPOOL_CONFIG_TYPE, VDEV_TYPE_ROOT);
    fnvlist_add_nvlist_array(root, ZPOOL_CONFIG_CHILDREN,
        (const nvlist_t **)child, n);
    return (root);
}

int
main(int argc, char **argv)
{
    static char cache[MAXPATHLEN];
    nvlist_t *child[WIDTH];
    nvlist_t *raidz;
    nvlist_t *props;
    spa_t *spa;

    if (argc != 2 || argv[1][0] != '/') {
        (void) fprintf(stderr, "usage: twp_create /absolute/dir\n");
        return (2);
    }
    /* Keep libzpool away from /etc/zfs/zpool.cache. */
    (void) snprintf(cache, sizeof (cache), "%s/none.cache", argv[1]);
    spa_config_path = cache;
    kernel_init(SPA_MODE_READ | SPA_MODE_WRITE);

    raidz = fnvlist_alloc();
    props = fnvlist_alloc();
    for (int i = 0; i < WIDTH; i++)
        child[i] = file_vdev(argv[1], i);
    fnvlist_add_string(raidz, ZPOOL_CONFIG_TYPE, VDEV_TYPE_RAIDZ);
    fnvlist_add_uint64(raidz, ZPOOL_CONFIG_NPARITY, 1);
    fnvlist_add_nvlist_array(raidz, ZPOOL_CONFIG_CHILDREN,
        (const nvlist_t **)child, WIDTH);
    nvlist_t *root = root_vdev(&raidz, 1);

    for (int i = 0; i < SPA_FEATURES; i++) {
        char *buf;
        VERIFY3S(-1, !=, asprintf(&buf, "feature@%s",
            spa_feature_table[i].fi_uname));
        fnvlist_add_uint64(props, buf, 0);
        free(buf);
    }
    fnvlist_add_string(props, zpool_prop_to_name(ZPOOL_PROP_CACHEFILE),
        "none");
    VERIFY0(spa_create(POOL, root, props, NULL, NULL, NULL));
    VERIFY0(spa_open(POOL, &spa, FTAG));

    vdev_t *rz = spa->spa_root_vdev->vdev_child[0];
    (void) printf("created: width %llu ashift %llu\n",
        (u_longlong_t)rz->vdev_children, (u_longlong_t)rz->vdev_ashift);

    nvlist_t *leaf = file_vdev(argv[1], WIDTH);
    nvlist_t *newroot = root_vdev(&leaf, 1);
    int error = spa_vdev_attach(spa, rz->vdev_guid, newroot, B_FALSE,
        B_FALSE);
    (void) printf("attach: %d\n", error);
    (void) fflush(stdout);

    /* With the bypass, the syncing thread aborts the process here. */
    txg_wait_synced(spa_get_dsl(spa), 0);
    (void) printf("no abort: the expansion was not started\n");
    spa_close(spa, FTAG);
    VERIFY0(spa_export(POOL, NULL, B_FALSE, B_FALSE));
    kernel_fini();
    return (3);
}
```

## STEP 3: make the pool (user space, no module loaded)

Once `$L/build-m.log` ends `build-rc=0` (the f build may still run), as your login user, not root:

```sh
D=/var/tmp/task-1121-twp; L=/var/log/task-1121-twp
mkdir $D/bin $D/vdev $D/vdev-nb
cd $D/m && ./libtool --mode=link --tag=CC gcc -std=gnu99 -g -O0 -Wall -pthread -include ./zfs_config.h -include ./include/sys/simd_config.h -I./include -I./lib/libspl/include -I./lib/libspl/include/os/linux -I./lib/libzpool/include -D_GNU_SOURCE -D_REENTRANT -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE -DDEBUG -UNDEBUG -DZFS_DEBUG -DTEXT_DOMAIN='"zfs-linux-user"' -o $D/bin/twp_create $L/twp_create.c libzpool.la libzfs_core.la libnvpair.la > $L/twp_create-build.txt 2>&1; echo "build-rc=$?"; grep -v '^libtool: link' $L/twp_create-build.txt
(ulimit -c 0; cd $D && $D/bin/twp_create $D/vdev-nb > $L/create-nb.txt 2>&1; echo "exit=$?" >> $L/create-nb.txt); cat $L/create-nb.txt; rm -rf $D/vdev-nb
(ulimit -c 0; cd $D && TWP_OLD_ATTACH=1 $D/bin/twp_create $D/vdev > $L/create.txt 2>&1; echo "exit=$?" >> $L/create.txt); cat $L/create.txt
ls $D/vdev | wc -l; du -sh $D/vdev
$D/m/zdb -l $D/vdev/d56 > $L/label-d56.txt 2>&1; grep -cE "^ +type: 'file'" $L/label-d56.txt; grep -c raidz_expanding $L/label-d56.txt
```

Expected: `build-rc=0` and nothing else from the link. The run without the variable (`create-nb.txt`): master refuses the attach, with ZFS_ERR_RAIDZ_EXPAND_TOO_WIDE:

```
created: width 56 ashift 16
attach: 1055
no abort: the expansion was not started
exit=3
```

The run with it (`create.txt`), as in main's run:

```
created: width 56 ashift 16
attach: 0
ASSERT at module/zfs/vdev_raidz.c:4783:raidz_reflow_scratch_sync()
VERIFY3U(write_size, >=, raidvd->vdev_children << ashift) failed (3670016 >= 3735552)
  PID: <n>     COMM: twp_create
  TID: <n>     NAME: txg_sync_thread
Call trace:
  ... libzpool.so.7 frames through dsl_sync_task_sync, dsl_pool_sync, spa_sync ...
exit=134
```

Then `57` files (d00 to d56, nothing else), a few tens of MiB, and the label of d56 with `57` file children and `1` raidz_expanding. Any other result: stop and report it. The pool is now in `$D/vdev`, never exported.

## STEP 4: part A, master's module, read-write import

```sh
D=/var/tmp/task-1121-twp
sudo -n $D/m/scripts/zfs.sh && cat /sys/module/zfs/srcversion && sudo -n dmesg | grep 'ZFS: Loaded module' | tail -n 1
timeout 30 sudo -n $D/m/zpool list -H -o name
```

The srcversion must equal SM, the load line say "(DEBUG mode)", and `zpool list` print nothing. Note N0=`sudo -n dmesg | wc -l`. Write $L/progress.md: "TASK-1121-TWP: steps 0-3 done; step 4 import started; next: step 5 reboot, then step 6".

```sh
D=/var/tmp/task-1121-twp; L=/var/log/task-1121-twp
setsid nohup sh -c "sudo -n $D/m/zpool import -f -N -o cachefile=none -d $D/vdev tw; echo import-rc=\$?" > $L/import-a.txt 2>&1 < /dev/null &
```

(`-f` because the pool was never exported.) Poll every 10 s, at most 3 minutes: `sudo -n dmesg | tail -n +$((N0+1)) | grep -E 'VERIFY3U|PANIC at'` and `cat $L/import-a.txt`. Expected within seconds:

```
VERIFY3U(write_size, >=, raidvd->vdev_children << ashift) failed (3670016 >= 3735552)
PANIC at vdev_raidz.c:4783:raidz_reflow_scratch_sync()
```

followed by `Showing stack for process <pid>` and a Call Trace through `spl_panic`, `raidz_reflow_scratch_sync`, `dsl_sync_task_sync`, `dsl_pool_sync`, `spa_sync`, `txg_sync_thread`. `$L/import-a.txt` either stays empty (the import waits in `spa_import()` -> `spa_config_update()` for a txg that never syncs, holding the namespace lock) or shows `import-rc=0` (if that txg synced just before the one carrying the scratch phase); either is fine, and in both cases the pool is wedged.

30 s after the PANIC line, capture into $L/hang-a.txt: `ps -eLo pid,tid,stat,comm,wchan:32 | grep -E ' D|txg|zpool|raidz'`, `sudo -n cat /proc/<tid>/stack` of every D task (txg_sync and the zpool process at least), and save `sudo -n dmesg | tail -n +$((N0+1)) > $L/dmesg-a.txt`. Quote: the two lines above, the first 12 frames of the Call Trace, the /proc stacks of txg_sync and of zpool, `cat $L/import-a.txt`, and the seconds from the import's start to the PANIC line.

No VERIFY after 3 minutes (whether or not the import returned) is a surprise: capture as above, touch nothing else, stop, report and wait for main (no reboot). An `import-rc` other than 0 without a VERIFY: quote it, unload the modules (`sudo -n $D/m/scripts/zfs.sh -u`), stop and report.

## STEP 5: the reboot (the one pre-approved)

Complete $L/progress.md ("step 4 done, pool wedged as expected; next: step 6"), `sync -f $L/progress.md`, arrange your own resumption about 5 minutes later (say which), then `sudo -n systemd-run --on-active=15 /bin/sh -c 'echo b > /proc/sysrq-trigger'`. Note the time.

After the reboot: read $L/progress.md; `uptime -s`, `uname -r` (as in step 0), `lsmod | grep -E '^(zfs|spl) '` (empty), `ls -l /dev/zfs 2>&1` (absent), `sysctl kernel.panic_on_oops kernel.hung_task_panic` (back to step 0's values; if step 0 set hung_task_panic to 0, set it to 0 again and say so), `test ! -s /etc/zfs/zpool.cache && echo CACHE-OK` (nothing was written: cachefile=none), `ls $D/vdev | wc -l` (57).

## STEP 6: part B, the fix

```sh
D=/var/tmp/task-1121-twp
sudo -n $D/f/scripts/zfs.sh && cat /sys/module/zfs/srcversion && sudo -n dmesg | grep 'ZFS: Loaded module' | tail -n 1
```

srcversion equal to SF, "(DEBUG mode)". Note N1=`sudo -n dmesg | wc -l`.

B1, the first import (`-f`: the master's import never exported it):

```sh
D=/var/tmp/task-1121-twp; L=/var/log/task-1121-twp
setsid nohup sh -c "sudo -n $D/f/zpool import -f -N -o cachefile=none -d $D/vdev tw; echo import-rc=\$?" > $L/import-b1.txt 2>&1 < /dev/null &
```

Poll `cat $L/import-b1.txt` every 5 s, at most 120 s. Expected `import-rc=0` within about 10 s and nothing else in the file; in dmesg since N1 exactly one line `WARNING: pool 'tw': raidz vdev 0 is too wide to be expanded to 57 children, its expansion is stopped`, and no VERIFY, ASSERT or PANIC.

B2, what the pool shows:

```sh
D=/var/tmp/task-1121-twp; L=/var/log/task-1121-twp; F=$D/f
timeout 60 sudo -n $F/zpool status -v tw | tee $L/status-b1.txt
timeout 60 sudo -n $F/zpool history -i tw | grep -E 'reflow too wide|import|open'
timeout 60 sudo -n $F/zpool events -v | grep -c 'reflow too wide'; sudo -n grep -c 'reflow too wide' /proc/spl/kstat/zfs/dbgmsg
timeout 30 sudo -n $F/zpool wait -t raidz_expand tw; echo "wait-rc=$?"
truncate -s 256M $D/vdev/d57
timeout 60 sudo -n $F/zpool attach tw raidz1-0 $D/vdev/d57; echo "attach-rc=$?"
timeout 60 sudo -n $F/zpool checkpoint tw; echo "checkpoint-rc=$?"
rm -f $D/vdev/d57
```

Expected: `state: ONLINE`; `expand: expansion of raidz1-0 in progress since <date>`, then a line ending `0.00% done, (copy is slow, no estimated time)`; raidz1-0 with 57 file children d00 to d56, all ONLINE; `errors: No known data errors`. In the history one `[txg:<n>] reflow too wide vdev=0 width=57` line (quote the lines shown). At least `1` history event and `1` dbgmsg line. `wait-rc=124` (still waiting after 30 s). The attach and the checkpoint refused with rc 1, expected message `raidz expansion in progress`; any refusal is acceptable, quote it; an attach that succeeds is a finding: stop and report.

B3, I/O:

```sh
D=/var/tmp/task-1121-twp; L=/var/log/task-1121-twp; F=$D/f
timeout 60 sudo -n $F/zfs create -o mountpoint=$D/mnt tw/fs; echo "create-rc=$?"
timeout 300 sudo -n dd if=/dev/urandom of=$D/mnt/f1 bs=1M count=512 status=none; echo "dd-rc=$?"
timeout 120 sudo -n $F/zpool sync tw; echo "sync-rc=$?"
sudo -n sha256sum $D/mnt/f1 | tee $L/f1.sha256
timeout 60 sudo -n $F/zpool list -v tw; timeout 60 sudo -n $F/zfs list -r -o name,used,avail,refer,mountpoint tw
timeout 900 sudo -n $F/zpool scrub -w tw; echo "scrub-rc=$?"
timeout 60 sudo -n $F/zpool status -v tw | tee $L/status-b3.txt
```

Expected: every rc 0; the scan line `scrub repaired 0B in <time> with 0 errors`, no error count on any device, and the expand line still at 0.00%. A non-zero rc here: quote it, skip the rest of B3, go on with B4.

B4, export and import again:

```sh
D=/var/tmp/task-1121-twp; L=/var/log/task-1121-twp; F=$D/f
timeout 120 sudo -n $F/zpool export tw; echo "export-rc=$?"
setsid nohup sh -c "sudo -n $F/zpool import -N -o cachefile=none -d $D/vdev tw; echo import-rc=\$?" > $L/import-b2.txt 2>&1 < /dev/null &
```

Poll as in B1 (no `-f` now: the pool was exported). Expected `export-rc=0`, `import-rc=0`, and a second `is too wide` warning in dmesg (two since N1). Then:

```sh
D=/var/tmp/task-1121-twp; L=/var/log/task-1121-twp; F=$D/f
timeout 60 sudo -n $F/zfs mount tw/fs; echo "mount-rc=$?"
sudo -n sha256sum -c $L/f1.sha256
timeout 300 sudo -n dd if=/dev/urandom of=$D/mnt/f2 bs=1M count=64 status=none; echo "dd-rc=$?"
timeout 120 sudo -n $F/zpool sync tw; echo "sync-rc=$?"
timeout 60 sudo -n $F/zpool history -i tw | grep -c 'reflow too wide'
timeout 60 sudo -n $F/zpool status tw | grep -A1 'expand:'
timeout 120 sudo -n $F/zpool export tw; echo "export-rc=$?"
```

Expected: rc 0 everywhere, `f1: OK`, `2` history records, the expansion still at 0.00%.

B5, the exported pool checked in user space, as your login user:

```sh
D=/var/tmp/task-1121-twp; L=/var/log/task-1121-twp
timeout 900 $D/f/zdb -e -p $D/vdev -bcc tw > $L/zdb-bcc.txt 2>&1; echo "zdb-rc=$?"
grep -E 'leak|No leaks|checksum|error' $L/zdb-bcc.txt
```

Expected `zdb-rc=0` and `No leaks (block sum matches space maps exactly)`, no checksum error.

After B5: every line of `sudo -n dmesg | tail -n +$((N1+1))` matching `VERIFY|ASSERT|PANIC|BUG|Oops|WARNING|blocked for more than` quoted with 5 lines before it (only the two `is too wide` warnings expected). `timeout 30 sudo -n $F/zpool list -H -o name` (empty), then `sudo -n $D/f/scripts/zfs.sh -u` (quote rc) and `lsmod | grep -E '^(zfs|spl) '` (empty). `sudo -n dmesg > $L/dmesg-b.txt`.

## STEP 7: cleanup

`sudo -n rm -rf $D`, `sudo -n fstrim -av`, `df -h /var/tmp`. `ls -l /etc/zfs/zpool.cache 2>&1`: absent expected; size 0: `sudo -n mv /etc/zfs/zpool.cache $L/zpool.cache.end`; above 0: report it and leave it. `sysctl kernel.hung_task_panic` back to step 0's value (if this task set it to 0 after the reboot, set it back and say so). Logs, the patch and the program stay in $L.

## Report

Write $L/report.md and give the same text as your final message:

1. Environment (VM, kernel, nproc, RAM, sysctls, zpool.cache at steps 0 and 7); TREES-OK and the hashes; builds (rc, warnings), SM, SF, DEBUG mode.
2. The pool: `create-nb.txt` and `create.txt` (quoted), the 57 files, the label counts.
3. Part A: the VERIFY and PANIC lines, seconds from import to PANIC, the first 12 frames, the /proc stacks of txg_sync and zpool, `import-a.txt`; the reboot (time, how you resumed).
4. Part B: one table (check | expected | seen) for B1 to B5: import rc and warning count, status expand line, history, events, dbgmsg, wait-rc, attach-rc and checkpoint-rc with messages, create/dd/sync/scrub rc and scan line, export/import rc, sha256 check, second warning, history count 2, zdb rc and leak line; then the dmesg check.
5. One verdict line, e.g. "M-43 fix cdccdbee036 on 6.12 debug: master wedges txg_sync on the read-write import (VERIFY at vdev_raidz.c:4783); the fix parks the expansion: import, I/O, scrub, export and re-import clean, warning once per import, status 0.00%, wait pending, zdb no leaks", or what differs.
6. Deviations, cleanup, then End.
