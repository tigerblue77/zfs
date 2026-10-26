# VM 107: kernel tests of the encryption key cache and the dedup BLAKE3 IV

These instructions are for the session on VM 107 (Debian 13, kernel
`6.12.113+deb13-cloud-amd64`, 16 CPUs, Xeon E5-2696 v4: AES-NI and AVX2, no
SHA-NI, no AVX-512). They give two fork branches of `tigerblue77/zfs` what
only a kernel can show. Both sit on upstream master
`a128b8a075b5b13c737e56914cd7043bd3e9ebf8` ("Linux 7.3 compat: META"), and
neither has loaded in a kernel before:

- **KC**, `zio-crypt-salt-key-cache` at
  `d511c2638b3e0a20e4a039932150a0d5e24a3027`: `93f2c3c24b` zeroes the derived
  key after a QAT operation, `10b18aa236` leaves a failed FreeBSD crypto
  session as it was, and `d511c2638b` gives each loaded key a cache of the
  keys derived for up to 16 salts other than the current one. A salt is
  admitted on its second decryption miss among the last 32 missed salts (a
  ghost list under its own mutex, `zsc_ghost_lock`), slots are read under an
  rwlock `zsc_lock` and replaced by CLOCK, entries are reference counted, and
  a new kstat `zio_crypt_stats` counts `salt_cache_hits`, `_misses`,
  `_admissions` and `_evictions`. Nothing changes on disk. 7 files, 457
  insertions, 19 deletions; no file under `tests/`.
- **IV**, `dedup-iv-blake3` at `6fcd458a731aa2be6d012616747ccbde0bf11f6f`:
  `e5a8ea7426` points a new key's HMAC key at its key bytes (with a unit
  test), `990286efdb` derives the salt and IV of encrypted dedup blocks with
  keyed BLAKE3 in keys created from now on (a new `uint64` entry
  `DSL_CRYPTO_DEDUP_PRF` = 1 in the DSL Crypto Key ZAP object, written when
  the new tunable `zfs_crypt_dedup_blake3` is 1, its default, and
  `feature@blake3` is enabled; carried by raw send and receive; read at key
  load, absent or unknown meaning HMAC-SHA512 as before), and `6fcd458a73`
  adds the ZTS test `functional/dedup/dedup_encrypted`, never run until now.
  16 files, 754 insertions, 21 deletions.

The run has four parts, each with its own results directory under
`/var/log/zts-enc-1026`, in this order:

| Part | Directory | What it establishes | Trees |
| --- | --- | --- | --- |
| A | `$L/A` | KC in the kernel: the kstat and its counters on data written under rotating salts and read after a reimport (KC, and M for the same data with no kstat), then 42 ZTS runs reaching the changed paths, compared result by result with M | KC, M |
| C | `$L/C` | IV in the kernel: `dedup_encrypted` (on IV, and on M with the test copied in), 36 runs of the dedup, key, raw send and receive, import and create tests against M, and pools written by each tree read, scrubbed and written again by the other | IV, M |
| B | `$L/B` | KC's locks under 16+ threads on one key: random reads of old-salt data, of dedup data (a salt per block, never cached) and of both at once, release builds, 5 rounds | KC-rel, M-rel |
| D | `$L/D` | IV's one unexplained userspace finding: the ztest invocation that once ended with a `zdb -y` livelist error, 3 times on M and 3 on IV | M, IV (userspace; M's module loaded) |

A stop in one part ends that part only: the next part starts from its own
module load. Two things end every part: a kernel panic, VERIFY, ASSERT, BUG,
oops or hung task (`$L/PANIC`), and anything left that may hold the module
(`$L/HELD`). Part C reuses part A's results on M for the labels both parts
run, and runs on M whatever part A did not finish.

| Tree | Path | Commit | Build | srcversion |
| --- | --- | --- | --- | --- |
| `KC` | `/var/tmp/zts-enc-1026/KC` | `d511c2638b` | `--enable-debug`, built | `3B1A49E4C8B4EAFE06AC02A` |
| `IV` | `/var/tmp/zts-enc-1026/IV` | `6fcd458a73` | `--enable-debug`, built | `A8DA0D31A506E045083D17C` |
| `M` | `/var/tmp/zts-algo-1026/M` | `a128b8a075` | `--enable-debug`, built, used by earlier tasks | `8852710DB871A7587B9E722` |
| `KC-rel` | `/var/tmp/zts-enc-1026/KC-rel` | `d511c2638b` | release, built | as KC's |
| `M-rel` | `/var/tmp/zts-enc-1026/M-rel` | `a128b8a075` | release, built | as M's |

All five were built in place before this document (`./autogen.sh &&
./configure [--enable-debug] && make -j8`): rc 0, no compiler warning. Do not
rebuild any. `srcversion` hashes the C sources only, so a release tree has
its debug twin's; the guard tells them apart the way VM 105's did, by
`zfs_flags` right after the load (4294932990 in a debug build, 0 in a
release one) and by the module's `coresize`, which must not change between
loads of one tree. It also checks what each code alone has: KC's kstat on
KC's two trees only, IV's tunable on IV only.

## The tests, and why these

**Part A, list A (42 runs, 185 result lines on KC and on M).** From the
branch note, the tests that write or read encrypted data, so that they reach
`zio_do_crypt_data()` with salts other than the current one. The list is the
one VM 105 used for the first version of the branch, plus `functional/crypto`:

| Label | Run | What it reaches |
| --- | --- | --- |
| `kg` | `-T zfs_load-key,zfs_unload-key,zfs_change-key` (26 lines) | key loads and unloads (the cache is created at unwrap and freed with the key), reads after a reload: the main path |
| `r01`-`r15` | 15 `rsend` tests alone (3 lines each) | raw sends and receives keep the sender's salts; `r01` reads back an encrypted zvol received from a deduplicated stream, `r02` writes with `dedup=on` (a salt per block: what the admission filter is for) |
| `c01`-`c10` | 10 `zfs_receive` tests alone | 7 raw or encrypted receives, 3 corrective receives (re-encryption with each block's own salt) |
| `l2` | `l2arc/persist_l2arc_002_pos` | L2ARC on an encrypted dataset: re-encryption with the block's salt, L2ARC reads, a reimport; needs fio |
| `s1`, `s2` | `zpool_scrub_encrypted_unloaded`, `zpool_scrub_thorough` | the scrubs with encrypted data; the thorough one decrypts |
| `i1`-`i5` | 5 `zpool_import` tests | import, key load, reads of what an earlier import wrote; `errata3`/`errata4` import images written by older code |
| `k1`, `k2` | `block_cloning_cross_enc_dataset`, `_replay_encrypted` | cloned blocks read through another dataset; ZIL replay on an encrypted dataset |
| `z1`, `z2` | `zdb_encrypted`, `zdb_encrypted_raw` (1 line each) | `zdb -K`: the branch's code in userspace, decrypting what the kernel wrote |
| `xg`, `sg` | `-T send_xdr_encoding` (16), `-T zfs_send` (15) | raw sends of encrypted datasets among them |
| `cr` | `-T crypto` (2) | the ICP's AES-CCM and AES-GCM test vectors (userspace; unchanged code: a cheap control) |
| `dg` | `-T dedup` (19) | a control: no test of the group creates an encrypted dataset at master |

**Part A's salt check.** The branch note asks to see the kstat count real
hits on data under older salts. On the loaded tree, pool `kcsalt` (one sparse
1 GiB file, no cache file) gets one encryption root (`recordsize=128k`,
`primarycache=metadata` so that every read decrypts, `compression=off`), and
20 files of 8 MiB (1280 data blocks) are written with
`zfs_key_max_salt_uses` at 64: every 64 encrypted block writes start a new
salt, so the data is spread over at least 20 salts, more than the cache's 16
slots. The parameter is put back, the pool exported and imported and the key
loaded (a new current salt: every block on disk is now under an older one),
and all 20 files are read twice, with the kstat read before and after each
pass. On KC each pass should add at least 1000 hits (a file's 64 blocks
share one or two salts: two misses each, the second admits, then hits), at
least 16 admissions and at least one eviction (more salts than slots), and
far fewer misses than hits; every file reads back as written. On M the same
workload runs with no kstat: it is the reference that the data reads back.

**Part C, list C (36 runs; 144 result lines on IV, 143 on M).**

| Label | Run | Why |
| --- | --- | --- |
| `de` | `dedup/dedup_encrypted` alone (3 lines) | the new test: copies on a BLAKE3 key and an HMAC key, in clones, after a reload, in raw-received copies; 8 duplicate and 0 unique DDT entries after each step |
| `dg` | `-T dedup` (20 on IV, which lists `dedup_encrypted`; 19 on M) | the whole group with the new code on every dedup write |
| `kg` | as in part A (26) | every key load now reads the new entry and derives the BLAKE3 key |
| `p1`, `p2`, `f1`, `f2` | `zpool_create_crypt_combos`, `zpool_create_encrypted`, `zfs_create_crypt_combos`, `zfs_create_encrypted` | key creation writes the entry; an encrypted pool root gets it in `spa_create()` |
| `r01`-`r15`, `c01`-`c07` | as in part A | raw send carries the entry, raw receive stores it |
| `i1`-`i5`, `z1`, `z2` | as in part A | keys of older pools (no entry); zdb on the keys |

Labels shared with list A name the same run: on M, part C takes part A's
result for each of them that ran there in full, and runs the rest (`de`,
`p1`, `p2`, `f1`, `f2`, and whatever part A did not reach).

`dedup_encrypted` on M runs with IV's two test files copied into M
(`dedup_encrypted.ksh` and the `tunables.cfg` that names
`zfs_crypt_dedup_blake3`) and restored right after. M's module has no such
tunable, so the test is expected to FAIL at its first
`set_tunable32 CRYPT_DEDUP_BLAKE3 1`, before writing anything, and to clean
up. That shows the test depends on the branch and cannot pass on master.

**Part C's cross-version check** (from the branch note's cross-release list,
with master standing for the older release). Two pools, each on a sparse
1 GiB file, no cache file, every copy made of one 2 MiB random file `src`
(16 blocks of 128 KiB), datasets with `dedup=sha256 compression=off
recordsize=128k xattr=sa`, raw keys from one key file:

| Phase | Tree | Pool `xvb` (created by IV) | Pool `xvm` (created by M) |
| --- | --- | --- | --- |
| xv1 | IV | create; `enc` (its key records BLAKE3); `file1`, `file2`; raw stream `iv-raw.zsend` | - |
| xv2 | M | read `file1`-`2`, scrub, write `file3` (HMAC: blocks of its own) | create; `enc` (no record); `file1`, `file2`; receive `iv-raw.zsend` as `fromiv` (M drops the record); raw stream `m-raw.zsend` |
| xv3 | IV | read `file1`-`3`, write `file4` (BLAKE3 again: dedups against `file1`-`2`, not `file3`), receive `m-raw.zsend` as `fromm` (no record), scrub | read all, write `file3` (a key without the record: HMAC, so it dedups against M's `file1`-`2`), scrub |
| xv4 | M | read all, scrub | read all, scrub |

After each phase the pool is exported and the loaded tree's `zdb` checks,
with `zfs_dedup_log_txg_max` set to 1 while the pool was imported (as
`dedup_encrypted` does, so that every entry reaches the DDT itself): the
duplicate, unique and log entries of the DDT, the number of keys recording
`DSL_CRYPTO_DEDUP_PRF = 1`, and `zdb -bcc`. Every copy is compared with
`src` by sha256.

| After | `xvb` duplicate / unique / keys with the record | `xvm` duplicate / unique / keys with the record |
| --- | --- | --- |
| xv1 | 16 / 0 / 1 | - |
| xv2 | 16 / 16 / 1 | 16 / 0 / 0 |
| xv3 | 16 / 16 / 1 | 16 / 0 / 0 |
| xv4 | 16 / 16 / 1 | 16 / 0 / 0 |

**Part B** (the branch note's contention recipe, sized for this VM). Pool
`kcperf` on one sparse 6 GiB file, no cache file, one encryption root
(`recordsize=4k`, `primarycache=metadata`, `compression=off`), so that both
workloads use one key, and with it one `zsc_lock` and one `zsc_ghost_lock`:
`old/` gets 16 files of 64 MiB written with `dedup=off`, then `dedup=on` and
`dd/` gets 16 files of 64 MiB of fresh random blocks (a salt per block, from
the HMAC of its plaintext). Written once on M-rel, the key reloaded, and read
once for the reference hashes. Then 5 rounds, each loading M-rel and then
KC-rel afresh: import and key load (a new current salt: `old` is under one
older salt), 5 s warm-ups, then three measured 30 s phases of
`fio --rw=randread --bs=4k --numjobs=16 --iodepth=32 --ioengine=libaio`:
`old` alone, `dd` alone, and both at once (two fio processes, 32 jobs on 16
CPUs). Recorded per phase: IOPS, mean latency, `%sys`/`%usr`/`%steal` from
`/proc/stat`, page-ins, and on KC-rel the kstat's deltas. What it shows: on
`old`, KC-rel against M-rel (the cache's gain on one hot old salt); on `dd`,
whether the misses, which take `zsc_lock` as reader and `zsc_ghost_lock` on
every block, run as fast as on master; and beside `dd`, how much of its
alone rate `old` keeps on each tree (the first version of the branch lost
48-69% there in userspace, the current one should not). **This VM shares its
hypervisor with other guests: the numbers are indicative, and `%steal`
says how busy the host was.**

**Part D.** The branch's notes record one ztest run of IV (`ztest -V -T 300
-f <dir>`, default sizes, run as a user with no core files and
`LD_LIBRARY_PATH` at the tree's `.libs`) whose final `zdb -bcc -G -d -Y -e -y`
reported `ERROR: Livelist ALLOC [3db6c000:5000] from TXG 77f FREED at TXG
77f`, on an unencrypted dataset, not seen in 3 later IV runs; master reached
that check less often. Its header was `1 vdevs, 7 datasets, 23 threads, 16
draid disks, parity 1, 300 seconds...` and its seed
`bd69993f1c9411d6173f7cb3d98ee2cf38f25d228f9572275d4786a9cd2c68bf`. Here: 3
rounds of that invocation, M then IV, round 1 with `-J <that seed>` (ztest's
"fishing" option: the same pseudo-random choices, so the same pool layout;
IV's ztest draws one more number per encrypted dataset, so the two trees
diverge after the first one), rounds 2 and 3 with fresh seeds. M's module is
loaded meanwhile: libzpool's `pool_active()` opens `/dev/zfs`, and without it
ztest's import path aborts with "no pools found", which is what cut the notes'
runs short.

## Rules for the whole task

- Log in as `tigerblue77` (passwordless sudo, not root) and run everything as
  that user, in bash, in this order; use `sudo -n` only where it is written.
- Post nothing, push nothing, install nothing (no package, no `make
  install`), never run `scripts/zfs-helpers.sh`, never use `zfs-tests.sh -x`,
  never reboot, rebuild nothing.
- Work only under `/var/tmp/zts-enc-1026` (`$W`) and log to
  `/var/log/zts-enc-1026` (`$L`). In the shared master tree
  `/var/tmp/zts-algo-1026/M`, change only its runner's `BASEDIR` (redirected
  in STEP 1 and STEP 7, restored in STEP 13) and, during part C's one
  `dedup_encrypted` run on M, add IV's `dedup_encrypted.ksh` and IV's
  `tunables.cfg` (restored in the same block). Never touch other tasks'
  directories (`/var/tmp/test_results`, other `/var/tmp/zts-*`, other
  `/var/log/zts-*`).
- Outside `$W` and `$L`, the one other change is STEP 12's `chmod 0666
  /dev/zfs` (the mode the project's udev rule gives it), gone with the module.
- Never create, edit or remove `/etc/zfs/zpool.cache` or
  `/etc/zfs/vdev_id.conf` yourself. ZTS pools use the default cache file, so
  the kernel rewrites it while they exist; this task's own pools use none.
- Leave every file, process, tmux pane, loop device, pool and module you did
  not create alone. Never run a `zpool` or `zfs` command while none of this
  task's modules is loaded. Leave no background process behind.
- Never print the environment, a token or a key file (`env`, `printenv`,
  `set`, `/proc/*/environ`, `$W/*/key`).
- A production hypervisor sits underneath: module loads, tests, fio and
  ztest run under `nice -n 10 ionice -c3`; one thing at a time.
- From STEP 1 on, every block starts by sourcing `/var/log/zts-enc-1026/env.sh`
  and naming its part, so that it works in a shell that keeps no state.
- STEPS 3, 4, 6, 10, 11 and 12 run for 10 to 45 minutes each. Run each block
  whole; if your shell tool cannot wait that long in the foreground, start
  that one block with the tool's own background mode and wait for it to exit,
  following `$L/<part>/runs.txt`. Never use `&` or `nohup` yourself, and run
  nothing meanwhile. A block is finished when its `STEP <n> done` or `STEP
  <n> stopped` line is in `$L/steps.txt`.
- **Any value other than the one written next to a check means stop**, unless
  the text says it is recorded only. A stop is written by the helpers to
  `$L/<part>/STOP` and ends that part; go on with the next step, whose
  helpers skip what must not run. A failed or skipped test is a result, not a
  stop. A kernel panic, oops, VERIFY, ASSERT or hung task means `$L/PANIC`:
  run nothing but STEP 13, which leaves the module loaded, and never reboot:
  the maintainer decides about the reboot.
- If your permission system refuses a command, record it as a deviation and
  go on where the text allows it; never work around it.
- Your session cannot run anything after its final reply. Everything,
  cleanup included, happens before it; the reply is the whole of
  `$L/REPORT.txt`.

What earlier VM runs taught, and the blocks already handle:

- `modinfo` is in `/usr/sbin`, not in `tigerblue77`'s PATH: every block puts
  `/usr/sbin:/sbin` first. This VM's umask is 0002; `env.sh` sets 022.
  `/var/log` directories are made with `sudo -n mkdir` and then given to
  `tigerblue77` (STEP 0 found `$L` already made).
- `configure --enable-debug` writes 3 `#define ZFS_DEBUG 1` lines, a release
  configure 1 (inside `#ifdef CONFIG_ZFS_DEBUG`). Compiler warnings are
  counted as `:[0-9]+:[0-9]+: warning:`; objtool prints about 83 harmless
  `warning:` lines per tree, which are not counted.
- The runner prints `Test: <path> (run as root) [mm:ss] [RESULT]` after a
  timestamp, `Test (Linux): ...` for a section of `linux.run`, and the path
  **with** `.ksh` for a test run alone; every form is matched and counted. A
  literal parenthesis in a regex is `[(]`.
- Kernel messages are read per run from `journalctl -k --since` (a failed
  test clears dmesg), without the runner's `ZTS run` markers and the
  `spa_import_progress_set_notes_impl` notes; a full stack depot's WARN splat
  (`slub_debug`) is folded into one line and is not a finding.
- The trees are not installed, so `compatibility.d` is absent (no selected
  test sets `compatibility=`); a zvol test (`r01`, `r02`) may fail where no
  udev rule makes `/dev/zvol` links: such failures are recorded and compared
  with M, never a stop.
- Master's tests leak a few files into the `-d` directory, fixed on another
  branch and not in these trees: `vdevs_in_pool()`'s `tmp.*` files (root's,
  mode 600, first line `config:`), `removal_with_zdb`'s empty `tmp.*`, and
  `removal_condense_export`'s empty `testdir1`. The leftovers check records
  and removes exactly those, on every tree, and stops on anything else.
- `zfs-tests.sh -d` points at a mode 1777 directory; each run waits 2 s first
  (two runs started in one second would share a results directory).

## STEP 0: the VM must be free, and the trees as built

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-enc-1026; L=/var/log/zts-enc-1026; MT=/var/tmp/zts-algo-1026/M
{
date -u '+%F %T UTC'
id -un; uname -r; grep PRETTY_NAME /etc/os-release; nproc; umask
grep -m1 'model name' /proc/cpuinfo
echo "cpu flags: $(grep -o -w -E 'aes|pclmulqdq|avx2|avx512f|sha_ni' /proc/cpuinfo | sort -u | xargs)"
echo "cmdline: $(grep -o 'slub_debug=[^ ]*' /proc/cmdline)"
free -m | sed -n 1,2p
echo "/var/tmp free: $(df -BG --output=avail /var/tmp | tail -1 | tr -d ' ')"
lsmod | grep -E '^(zfs|spl) '
ls /sys/module | grep -xE 'zfs|spl'
ls -l /dev/zfs 2>&1
pgrep -af 'zfs-test[s]|test-runne[r]|[z]test|[z]loop|zfs[.]s[h]|[a]utogen|[c]onfigure|[m]ake|[g]cc|[c]c1|[k]sh|[z]db|[f]io '
pgrep -ax 'zed|lt-zed'
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
sudo -n true && echo "passwordless sudo ok"
echo "loops: [$(sudo -n losetup -l -n --raw -O NAME,BACK-FILE | xargs)]; dm: $(sudo -n dmsetup ls 2>&1)"
grep -E ' zfs |testpool' /proc/mounts
ls -d /testpool* 2>&1
ls -l /etc/zfs/zpool.cache /etc/zfs/vdev_id.conf /tmp/ZTS-snakeoil.log 2>&1
sudo -n journalctl -k -n 1 --no-pager -q
echo "stack depot warnings this boot: $(sudo -n journalctl -k -b --no-pager -q | grep -c 'Stack depot reached limit capacity')"
echo "modprobe options: $(grep -rhsE '^options +(zfs|spl) ' /etc/modprobe.d /usr/lib/modprobe.d /run/modprobe.d | xargs)"
echo "zvol udev rule: $(ls /etc/udev/rules.d/60-zvol.rules /usr/lib/udev/rules.d/60-zvol.rules /lib/udev/rules.d/60-zvol.rules 2>/dev/null | xargs)"
echo "core_pattern: $(cat /proc/sys/kernel/core_pattern)"
for c in ksh python3 git modinfo sha256sum timeout fio openssl xxh128sum bzcat mkfs.ext4 mpstat; do
	echo "$c: $(command -v $c || echo absent)"
done
echo "W holds: $(LC_ALL=C ls -A $W | xargs)"
echo "L holds: $(LC_ALL=C ls -A $L | xargs)"
stat -c '%U %a %n' $W $L $MT
cd $MT && echo "M: $(git rev-parse HEAD), tracked changes $(git status --short --untracked-files=no | wc -l)," \
    "srcversion $(modinfo -F srcversion module/zfs.ko)"
grep -n '^BASEDIR' $MT/tests/test-runner/bin/test-runner.py.in $MT/tests/test-runner/bin/test-runner.py
for t in KC IV KC-rel M-rel; do
	echo "$t: $(git -C $W/$t rev-parse HEAD 2>&1), srcversion $(modinfo -F srcversion $W/$t/module/zfs.ko 2>&1)"
done
} 2>&1 | tee $L/step0.txt
```

Expected: the date; `tigerblue77`, `6.12.113+deb13-cloud-amd64`, `Debian
GNU/Linux 13 (trixie)`, `16`, `0002`; `Intel(R) Xeon(R) CPU E5-2696 v4 ...`;
`cpu flags: aes avx2 pclmulqdq` (no `avx512f`, no `sha_ni`); the cmdline,
memory and free-space lines (recorded; at least `10G` free, `12G` when this
was written); nothing for the module, `/sys/module`, process and `zed` lines;
`No such file or directory` for `/dev/zfs`; only this session's tmux pane;
`passwordless sudo ok`; `loops: []; dm: No devices found`; no mount line; `No
such file or directory` for `/testpool*`, `vdev_id.conf` and
`ZTS-snakeoil.log`, and `zpool.cache` absent or 0 bytes; one kernel line; the
stack depot, modprobe, zvol rule and core_pattern lines (recorded; a
modprobe option that sets `zfs_flags` would make the guard fail); a path for
`ksh`, `python3`, `git`, `modinfo`, `sha256sum`, `timeout` and `fio`, a path
or `absent` for the others (recorded); `W holds: IV KC KC-rel M-rel repo`;
`L holds:` with `build-IV.log`, `build-KC.log`, `fetch.log` and the
`step0.txt` this block writes (release build logs recorded if there), and no
`env.sh`, `A`, `B`, `C`, `D` or `REPORT.txt`; the three directories `tigerblue77`'s;
`M: a128b8a075b5b13c737e56914cd7043bd3e9ebf8, tracked changes 0, srcversion
8852710DB871A7587B9E722`; line 43 of both runner files `BASEDIR =
'/var/tmp/test_results'`; then

```
KC: d511c2638b3e0a20e4a039932150a0d5e24a3027, srcversion 3B1A49E4C8B4EAFE06AC02A
IV: 6fcd458a731aa2be6d012616747ccbde0bf11f6f, srcversion A8DA0D31A506E045083D17C
KC-rel: d511c2638b3e0a20e4a039932150a0d5e24a3027, srcversion 3B1A49E4C8B4EAFE06AC02A
M-rel: a128b8a075b5b13c737e56914cd7043bd3e9ebf8, srcversion 8852710DB871A7587B9E722
```

Stop at once, changing nothing, if: the user is not `tigerblue77` or `sudo
-n` fails; another kernel; a zfs or spl module is loaded or `/dev/zfs`
exists; a test, build, fio, ztest or zdb process or any `zed` runs; another
tmux pane runs anything; a loop or dm device, a zfs or test mount, a
`/testpool*` directory, a non-empty `zpool.cache`, a `vdev_id.conf` or the
snakeoil log exists; less than 10G is free; `journalctl -k` prints nothing;
one of the tools with a required path is absent; `W holds` differs; `$L`
holds an `env.sh`, `A`, `B`, `C`, `D` or `REPORT.txt` (this task started
once already: report, do not start again); or any tree line differs. `fio`
absent would skip part B and `l2`: it is required here.

## STEP 1: environment, helpers, and the trees as built

This block writes `env.sh` (variables and every helper), `count.py` (counts
the result lines one run prints, from a tree's own runfiles), `table.py`
(part B's table) and the two lists, and makes this task's directories:

```sh
export PATH=/usr/sbin:/sbin:$PATH
L=/var/log/zts-enc-1026
if [ -e $L/env.sh ] || [ -e $L/A ]; then
echo "this task started already: stop"
else
cat > $L/env.sh <<'ENV'
umask 022
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-enc-1026
L=/var/log/zts-enc-1026
# The master debug tree, shared with earlier tasks: only its runner's
# BASEDIR and, in part C, two test files change, and both are restored.
MT=/var/tmp/zts-algo-1026/M
HKC=d511c2638b3e0a20e4a039932150a0d5e24a3027
HIV=6fcd458a731aa2be6d012616747ccbde0bf11f6f
HM=a128b8a075b5b13c737e56914cd7043bd3e9ebf8
SRCV_KC=3B1A49E4C8B4EAFE06AC02A
SRCV_IV=A8DA0D31A506E045083D17C
SRCV_M=8852710DB871A7587B9E722
# zfs_flags right after a load: every flag but DPRINTF, SET_ERROR,
# INDIRECT_REMAP and RAIDZ_RECONSTRUCT in a debug build, 0 in a release one.
FLAGS_dbg=4294932990
FLAGS_rel=0
# What tells the three codes apart once loaded: KC's kstat, IV's tunable.
KSTAT=/proc/spl/kstat/zfs/zio_crypt_stats
PIV=/sys/module/zfs/parameters/zfs_crypt_dedup_blake3
# The seed of the IV ztest run whose zdb -y reported the livelist error.
ZSEED=bd69993f1c9411d6173f7cb3d98ee2cf38f25d228f9572275d4786a9cd2c68bf
KRE='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
# A result line of the runner, "Test: <path> (run as root) [mm:ss] [RESULT]",
# or "Test (Linux): ..." for a section of linux.run.
TRE='Test( [(][^)]*[)])?: .* [(]run as [^)]*[)] +\[[0-9:]+\] \[[A-Z]+\]'
NRE='^ERROR: |cannot |unsupported|not supported|No such file|compatibility'
# Tunables the selected tests or this task change, and must find restored.
TUN="l2arc_noprefetch l2arc_rebuild_blocks_min_l2size zfs_scan_legacy
zfs_scan_suspend_progress zfs_dedup_log_txg_max zfs_dedup_log_flush_entries_min
zfs_dedup_log_flush_entries_max metaslab_force_ganging metaslab_force_ganging_pct
zfs_bclone_enabled zfs_disable_ivset_guid_check zfs_key_max_salt_uses zfs_flags
zfs_crypt_dedup_blake3 vdev_file_physical_ashift zfs_compressed_arc_enabled"
# Part B: parameters that must read the same at every load.
PP="zfs_key_max_salt_uses zfs_arc_max zfs_arc_min zfs_prefetch_disable
zfs_dbgmsg_enable zfs_flags zfs_dedup_prefetch icp_aes_impl icp_gcm_impl"
RT=30
FIO=$(command -v fio)

tdir() {   # tdir <tree>: its path
	case $1 in
	M) echo $MT ;;
	*) echo $W/$1 ;;
	esac
}

kind() {   # kind <tree>: dbg or rel
	case $1 in
	*-rel) echo rel ;;
	*) echo dbg ;;
	esac
}

# The part a block works for: its log directory PD and its ZTS -d
# directory FD (mode 1777).
part() {   # part <A|B|C|D>
	PT=$1
	PD=$L/$1
	FD=$W/files-$1
	mkdir -p $PD
}

same() {   # same <what> <got> <expected>
	if [ "$2" = "$3" ]; then
		echo "ok: $1"
	else
		echo "MISMATCH: $1: [$2], expected [$3]"
	fi
}

say() {
	echo "$*" | tee -a $PD/runs.txt
}

# A stop ends the current part: its STOP file names the first reason, and
# no helper of the part runs after it. PANIC (a kernel finding) and HELD
# (something left that may hold the module) end every part.
stop() {   # stop <reason>
	say "$*: stop"
	[ -e $PD/STOP ] || echo "$*" > $PD/STOP
	return 1
}

held() {   # held <reason>
	touch $L/HELD
	stop "$*"
}

blocked() {
	local f
	for f in $L/PANIC $L/HELD $PD/STOP; do
		if [ -e $f ]; then
			say "blocked by ${f#$L/} ($(cat $f 2>/dev/null)): nothing run"
			return 0
		fi
	done
	return 1
}

# The step's line in $L/steps.txt: "stopped" when its part stopped or a
# global flag is set, "done" otherwise.
step_end() {   # step_end <n>
	local r=done
	[ -e $PD/STOP ] && r=stopped
	{ [ -e $L/PANIC ] || [ -e $L/HELD ]; } && r=stopped
	echo "STEP $1 $r $(date '+%T')" | tee -a $L/steps.txt
}

tunables() {
	local t
	for t in $TUN; do
		printf '%s=%s ' $t "$(cat /sys/module/zfs/parameters/$t 2>/dev/null || echo absent)"
	done
}

loops() {
	sudo -n losetup -l -n --raw -O NAME,BACK-FILE 2>&1
}

kst() {   # kst: the four counters of KC's kstat, or "absent"
	if [ -r $KSTAT ]; then
		awk '$1 ~ /^salt_cache_(hits|misses|admissions|evictions)$/ { printf "%s ", $3 }' $KSTAT |
		    sed 's/ $//'
	else
		echo absent
	fi
}

# Test processes still running, leaving out this shell, its forks and its
# ancestors: their command lines can carry the words of the block.
others() {
	local skip me p rest q=$$
	skip=" $$ "
	while [ -n "$q" ] && [ "$q" -gt 1 ]; do
		q=$(awk '/^PPid:/ { print $2 }' /proc/$q/status 2>/dev/null)
		skip="$skip$q "
	done
	me=$(tr '\0' ' ' < /proc/$$/cmdline)
	pgrep -af 'zfs-test[s][.]sh|test-runne[r][.]py|zfs-tests/tests/functiona[l]/|[z]db |[z]inject |[f]io |[z]test |[h]ttp[.]server|[m]pstat' |
	    while read -r p rest; do
		case "$skip" in
		*" $p "*) continue ;;
		esac
		[ "$(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)" = "$me" ] && continue
		echo "$p $rest"
	done
}

# Kernel lines without what is known not to be a finding: the runner's
# "ZTS run" markers, the import progress notes, and a full stack depot's
# WARN splat (slub_debug), folded into one line.
kfilter() {
	awk '
	index($0, "ZTS run ") || index($0, "spa_import_progress_set_notes_impl") { next }
	index($0, "[ cut here ]") { inb = 1; n = 0; dep = 0 }
	inb {
		buf[++n] = $0
		if (index($0, "Stack depot reached limit capacity") || index($0, "lib/stackdepot.c"))
			dep = 1
		if (index($0, "[ end trace ")) {
			if (dep)
				print "DEPOT SPLAT (stack depot full, slub_debug; not ZFS): " n " lines"
			else
				for (i = 1; i <= n; i++) print buf[i]
			inb = 0
		}
		next
	}
	{ print }
	END { if (inb) for (i = 1; i <= n; i++) print buf[i] }'
}

# Kernel messages since <since>, from the journal (a failed test clears
# dmesg). A finding stops the part; a panic, VERIFY, ASSERT, BUG, oops or
# hung task also sets PANIC, which keeps the module loaded and ends all.
kern_check() {   # kern_check <label> <since>
	local n
	sudo -n journalctl -k --since "$2" --no-pager -q -o short-iso > $PD/kern-$1.txt 2>&1
	kfilter < $PD/kern-$1.txt > $PD/kernf-$1.txt
	n=$(grep -ciE "$KRE" $PD/kernf-$1.txt)
	say "kernel $1: $n findings, $(grep -c 'ZTS run ' $PD/kern-$1.txt) ZTS marks"
	grep -iE "$KRE" $PD/kernf-$1.txt | head -n 20 | sed 's/^/  /' | tee -a $PD/runs.txt
	if grep -qE 'PANIC|VERIFY|ASSERT|BUG:|Oops|blocked for more than' $PD/kernf-$1.txt; then
		touch $L/PANIC
		stop "$1: panic, assertion or hung task in the kernel log"
		return 1
	fi
	[ "$n" -eq 0 ] || { stop "$1: kernel finding"; return 1; }
}

# Load <tree>'s modules, after unloading whatever this task loaded. The
# guard: the module files are the ones STEP 1 hashed, the loaded srcversion
# is the tree's, zfs_flags says debug or release, the size matches every
# earlier load of the tree, KC's kstat exists on KC's trees only, IV's
# tunable on IV only, and no pool appears.
switch_to() {   # switch_to <tree>
	local t=$1 cur prev p f c k v e=
	for p in $L/PANIC $L/HELD; do
		[ -e $p ] && { say "blocked by ${p#$L/}: no module switch"; return 1; }
	done
	cur=$(cat /sys/module/zfs/srcversion 2>/dev/null)
	prev=$(cat $L/loaded-tree 2>/dev/null)
	if [ -n "$cur" ]; then
		if [ -z "$prev" ] || [ "$cur" != "$(cat $L/srcv-$prev)" ]; then
			held "a zfs module this task did not load is loaded ($cur)"
			return 1
		fi
		if [ "$prev" = "$t" ]; then
			say "$t already loaded"
			return 0
		fi
		(cd $(tdir $prev) && nice -n 10 ionice -c3 sudo -n ./scripts/zfs.sh -u) > $PD/unload-$prev.log 2>&1
		p=$?
		say "unload $prev rc=$p"
		[ $p -eq 0 ] || { held "unload of $prev failed"; return 1; }
		rm -f $L/loaded-tree
	fi
	if lsmod | grep -qE '^(zfs|spl) '; then
		held "zfs or spl still loaded"
		return 1
	fi
	if ! (cd $(tdir $t) && sha256sum -c --quiet $L/ko-$t.sha256); then
		stop "$t: module files differ from their record"
		return 1
	fi
	(cd $(tdir $t) && nice -n 10 ionice -c3 sudo -n ./scripts/zfs.sh) > $PD/load-$t.log 2>&1
	say "load $t rc=$?"
	cur=$(cat /sys/module/zfs/srcversion 2>&1)
	f=$(cat /sys/module/zfs/parameters/zfs_flags 2>&1)
	c=$(cat /sys/module/zfs/coresize 2>&1)
	[ -e $KSTAT ] && k=yes || k=no
	[ -e $PIV ] && v=yes || v=no
	[ "$cur" = "$(cat $L/srcv-$t)" ] || e="$e srcversion($cur)"
	[ "$(modinfo -F srcversion $(tdir $t)/module/zfs.ko)" = "$(cat $L/srcv-$t)" ] || e="$e file"
	[ "$f" = "$(eval echo \$FLAGS_$(kind $t))" ] || e="$e zfs_flags($f)"
	[ -s $L/coresize-$t ] || echo "$c" > $L/coresize-$t
	[ "$c" = "$(cat $L/coresize-$t)" ] || e="$e coresize($c, earlier $(cat $L/coresize-$t))"
	case $t in KC*) [ $k = yes ] || e="$e no-kstat" ;; *) [ $k = no ] || e="$e kstat" ;; esac
	case $t in IV*) [ $v = yes ] || e="$e no-IV-tunable" ;; *) [ $v = no ] || e="$e IV-tunable" ;; esac
	echo $t > $L/loaded-tree
	if [ -n "$e" ]; then
		held "$t GUARD FAILED:$e"
		return 1
	fi
	tunables > $L/tunables-loaded.txt
	say "$t guard ok: srcversion $cur, zfs_flags $f, coresize $c, kstat $k, IV tunable $v"
	p=$(cd $(tdir $t) && sudo -n ./zpool list -H -o name 2>&1)
	[ -z "$p" ] || { held "$t: pools present right after the load: $p"; return 1; }
}

# Before a run: nothing blocks, the loaded tree is <tree>, the label is new.
pre_run() {   # pre_run <label> <tree>
	blocked && return 1
	if [ "$(cat $L/loaded-tree 2>/dev/null)" != "$2" ] ||
	    [ "$(cat /sys/module/zfs/srcversion 2>&1)" != "$(cat $L/srcv-$2)" ]; then
		stop "$1: the loaded module is not $2's"
		return 1
	fi
	if [ -e $PD/$1.log ]; then
		say "label $1 already used: nothing run"
		return 1
	fi
}

# What a run left. Pools, devices, mounts and processes may hold the module
# (HELD). In FD, master's known test leaks are recorded and removed:
# vdevs_in_pool()'s files (tmp.*, root's, mode 600, first line "config:"),
# removal_with_zdb's empty tmp.*, and removal_condense_export's empty
# testdir1. Anything else there, or a tunable not restored, stops the part.
leftovers() {   # leftovers <label>
	local bad=0 hold=0 p e f known=
	{
	p=$(cd $(tdir $(cat $L/loaded-tree)) && sudo -n ./zpool list -H -o name 2>&1)
	[ -n "$p" ] && { echo "  LEFTOVER pools: $p"; hold=1; }
	[ "$(loops)" = "$(cat $L/loops-before.txt)" ] || { echo "  LEFTOVER loop devices: $(loops | xargs)"; hold=1; }
	[ "$(sudo -n dmsetup ls 2>&1)" = "$(cat $L/dm-before.txt)" ] || { echo "  LEFTOVER dm devices"; hold=1; }
	p=$(grep -E ' zfs |testpool' /proc/mounts)
	[ -n "$p" ] && { echo "  LEFTOVER mounts: $p"; hold=1; }
	p=$(others)
	[ -n "$p" ] && { echo "  LEFTOVER processes: $p"; hold=1; }
	p=$(pgrep -ax 'zed|lt-zed')
	[ -n "$p" ] && { echo "  LEFTOVER zed: $p"; hold=1; }
	p=$(ls -d /testpool* 2>/dev/null | xargs)
	[ -n "$p" ] && { echo "  LEFTOVER mount points: $p"; bad=1; }
	for f in /etc/zfs/vdev_id.conf /tmp/ZTS-snakeoil.log $(cat $L/zedpid.txt); do
		if [ -e $f ] || [ -L $f ]; then echo "  LEFTOVER $(ls -l $f)"; bad=1; fi
	done
	for e in $(LC_ALL=C ls -A $FD); do
		f=$FD/$e
		case $e in
		tmp.??????????)
			if [ -f $f ] && [ ! -L $f ] && [ "$(stat -c '%U %a' $f)" = "root 600" ] &&
			    { [ "$(stat -c %s $f)" = 0 ] || [ "$(sudo -n head -n 1 $f)" = "config:" ]; }; then
				known="$known $e($(stat -c %s $f)B)"
				sudo -n rm -f -- $f
				continue
			fi ;;
		testdir1)
			if [ -d $f ] && [ ! -L $f ] && [ "$(stat -c %U $f)" = root ] &&
			    [ -z "$(sudo -n ls -A $f)" ] && ! grep -q " $f " /proc/mounts; then
				known="$known testdir1"
				sudo -n rmdir -- $f
				continue
			fi ;;
		esac
		echo "  LEFTOVER in $FD: $(sudo -n ls -ld --time-style=+%T $f)"
		bad=1
	done
	[ -n "$known" ] && echo "  master's known test leaks, removed:$known"
	p=$(tunables)
	[ "$p" = "$(cat $L/tunables-loaded.txt)" ] || { echo "  LEFTOVER tunables: $p"; bad=1; }
	echo "  zpool.cache: $(stat -c '%s bytes' /etc/zfs/zpool.cache 2>/dev/null || echo absent)"
	echo "leftovers after $1: $([ $bad$hold = 00 ] && echo none || echo FOUND)"
	} > $PD/left-$1.txt 2>&1
	tee -a $PD/runs.txt < $PD/left-$1.txt
	[ $bad$hold = 00 ] && return 0
	[ $hold = 1 ] && touch $L/HELD
	stop "$1: leftovers"
}

# Result counts of runner result lines on stdin: "PASS x3", "PASS x18, SKIP x1".
tally() {
	grep -oE '(PASS|FAIL|SKIP|KILLED)\]( [(]RERAN[)])?$' | sed 's/\].*$//' |
	    sort | uniq -c | awk '{ printf "%s%s x%d", s, $2, $1; s = ", " }'
}

# "<RESULT> <dir/test>" for each result line (stdin) that did not pass.
notpassed() {
	grep -v '\[PASS\]' | sed -nE \
	    's|^.*Test( [(][^)]*[)])?: .*/tests/functional/([^ ]+) [(]run as [^)]*[)] +\[[0-9:]+\] \[([A-Z]+)\].*$|\3 \2|p' |
	    sed 's/\.ksh$//'
}

# For each run of test <name> that did not pass, from the runner's log: its
# result line, the lines matching NRE, the first failing line, the first
# ERROR line.
log_digest() {   # log_digest <results dir> <test name>
	awk -v n="$2" -v nre="$NRE" '
	function flush() {
		if (bad && !done) {
			print "    last line: " prev
			print "    first ERROR line: " (err != "" ? err : "none")
		}
	}
	/Test( [(][^)]*[)])?: .* [(]run as [^)]*[)] +\[[0-9:]+\] \[[A-Z]+\]/ {
		flush()
		sec = index($0, "/" n " (") || index($0, "/" n ".ksh (")
		bad = sec && $0 !~ /\[PASS\]/
		notes = 0; done = 0; prev = ""; err = ""
		if (bad) print "  " $0
		next
	}
	!sec { next }
	{ line = $0; sub(/^[0-9:.]+ /, "", line) }
	bad && !done && line ~ /NOTE: Performing (test-fail callback|local cleanup)/ {
		print "    first failing line: " prev
		print "    first ERROR line: " (err != "" ? err : "none")
		done = 1
	}
	bad && !done && err == "" && line ~ /^ERROR: / { err = line }
	bad && !done && line ~ nre && notes < 12 { print "    note: " line; notes++ }
	{ prev = line }
	END { flush() }' "$1/log"
}

# One zfs-tests.sh run on <tree> (loaded): <how> is -t with a test (<arg> =
# dir/test under tests/functional) or -T with tags. Its counts against
# <expected> result lines, each line that did not pass with its digest, the
# kernel messages and the leftovers. A failed test is a result, not a stop.
zts() {   # zts <label> <tree> <-t|-T> <arg> <expected lines>
	local label=$1 t=$2 since t0 rc d n c a r x k
	pre_run $label $t || return 1
	sleep 2   # two runs started in one second would share a results directory
	[ "$3" = -t ] && a=tests/functional/$4.ksh || a=$4
	since=$(date '+%F %T')
	t0=$(date +%s)
	(cd $(tdir $t) && timeout -k 60 $([ "$3" = -t ] && echo 1800 || echo 3600) nice -n 10 ionice -c3 \
	    ./scripts/zfs-tests.sh -vK -d $FD $3 $a) < /dev/null > $PD/$label.log 2>&1
	rc=$?
	c=$(grep -E "$TRE" $PD/$label.log | tally)
	n=$(grep -cE "$TRE" $PD/$label.log)
	say "$label $t $3 $4 ${c:-NO RESULT} ($n of $5 lines) rc=$rc wall=$(( $(date +%s) - t0 ))s"
	echo "$label ${c:-NO RESULT} lines=$n/$5 rc=$rc" >> $PD/zts-results.txt
	grep -E "$TRE" $PD/$label.log | notpassed > $PD/$label.notpassed
	grep -m 1 '^Missing util' $PD/$label.log | sed 's/^/  /' >> $PD/missing-utils.txt
	d=$(awk '/^Log directory/ { print $3 }' $PD/$label.log | head -n 1)
	while read -r -u 4 r x; do
		if [ -f "$d/log" ]; then
			log_digest "$d" "${x##*/}" | tee -a $PD/runs.txt
		else
			say "  $r $x: no runner log (Log directory: '$d')"
		fi
	done 4< $PD/$label.notpassed
	kern_check $label "$since"
	k=$?
	[ -e $L/PANIC ] && return 1
	leftovers $label || return 1
	[ $k = 0 ] || return 1
	[ $rc -ne 124 ] && [ $rc -ne 137 ] || { stop "$label: outer timeout"; return 1; }
	[ "$n" = "$5" ] || { stop "$label: $n result lines, expected $5"; return 1; }
}

# The expected result lines of each label of <list> on <tree>, from the
# tree's own runfiles (STEP 1 writes them).
nlines() {   # nlines <tree> <label>
	awk -v l=$2 '$1 == l { print $2 }' $PD/n-$1.txt
}

# The labels of <list> on <tree>, prefixed <p>. With "skip-done", a label
# already run on M by part A (all its lines there) is taken from part A.
run_list() {   # run_list <p> <tree> <list> [skip-done]
	local label how arg x
	while read -r -u 3 label how arg; do
		case $label in '#'*|'') continue ;; esac
		if [ -e $PD/$1-$label.log ]; then
			say "$1-$label: already run"
			continue
		fi
		if [ "$4" = skip-done ] &&
		    grep -qE "^m-$label .* lines=([0-9]+)/\1 " $L/A/zts-results.txt 2>/dev/null; then
			say "m-$label: taken from part A ($(grep "^m-$label " $L/A/zts-results.txt | cut -d' ' -f2-))"
			continue
		fi
		zts $1-$label $2 $how $arg "$(nlines $2 $label)" || return 1
	done 3< $3
	say "list $(basename $3) done on $2"
}

# Branch <p> against M (m-), label by label; M's result comes from this
# part or, for a label taken from part A, from part A. A test that fails on
# both is master's or this VM's; on the branch only, the branch's until
# shown otherwise.
compare_zts() {   # compare_zts <p> <list>
	local label how arg b m mf
	while read -r -u 3 label how arg; do
		case $label in '#'*|'') continue ;; esac
		mf=$PD
		[ -e $PD/m-$label.log ] || mf=$L/A
		printf '%-4s %-48s %s: %-22s M: %s\n' $label $arg $1 \
		    "$(sed -n "s/^$1-$label \(.*\) lines=.*$/\1/p" $PD/zts-results.txt)" \
		    "$(sed -n "s/^m-$label \(.*\) lines=.*$/\1/p" $mf/zts-results.txt 2>/dev/null)"
		b=$(LC_ALL=C sort -k2 $PD/$1-$label.notpassed 2>/dev/null)
		m=$(LC_ALL=C sort -k2 $mf/m-$label.notpassed 2>/dev/null)
		LC_ALL=C join -j 2 -o 0,1.1,2.1 <(echo "$b") <(echo "$m") | grep "[^ ]" |
		    sed 's/^/       on both: /'
		LC_ALL=C join -j 2 -v 1 -o 0,1.1 <(echo "$b") <(echo "$m") | grep "[^ ]" |
		    sed 's/^/       BRANCH ONLY: /'
		LC_ALL=C join -j 2 -v 2 -o 0,2.1 <(echo "$b") <(echo "$m") | grep "[^ ]" |
		    sed 's/^/       M only: /'
	done 3< $2
}

# Once more, alone, on the loaded branch <tree>, each test that FAILED or
# was KILLED there and passed on M (at most 6): labels <p>r-<label>-<i>.
rerun_branch_only() {   # rerun_branch_only <p> <tree> <list>
	local i=0 x r
	compare_zts $1 $3 | sed -n 's/^       BRANCH ONLY: \([^ ]*\) \(FAIL\|KILLED\)$/\1/p' > $PD/branch-only.txt
	while read -r -u 3 x; do
		i=$((i + 1))
		[ $i -le 6 ] || { say "$1r: $x not rerun (more than 6)"; continue; }
		r=$(python3 -I $L/count.py $(tdir $2) -t $x | cut -d' ' -f2)
		zts $1r-$i $2 -t $x "$r" || return 1
	done 3< $PD/branch-only.txt
	say "reruns on $2: $(grep -c . $PD/branch-only.txt) branch-only failures, $([ $i -gt 6 ] && echo 6 || echo $i) rerun"
}

# The runner's results directory of <tree>, pointed at <dir>; the files as
# built are kept in $L/orig-<tree> for STEP 13.
set_basedir() {   # set_basedir <tree> <dir>
	local r=tests/test-runner/bin/test-runner.py
	cd $(tdir $1) || return 1
	if [ ! -e $L/orig-$1/test-runner.py ]; then
		mkdir -p $L/orig-$1 && cp -p $r.in $r $L/orig-$1/ || return 1
	fi
	sed -i "s|^BASEDIR = .*|BASEDIR = '$2'|" $r.in $r
	same "$1 results directory $2" "$(grep -cx "BASEDIR = '$2'" $r) $(grep -cx "BASEDIR = '$2'" $r.in)" "1 1"
}

# Part A: on the loaded <tree>, pool kcsalt on one sparse 1 GiB file (no
# cache file), one encryption root written with zfs_key_max_salt_uses=64
# (a new salt about every 64 encrypted blocks: at least 20 for its 1280
# data blocks), the parameter restored, then export, import and key load
# (a new current salt, so every block on disk is under an older one) and
# two read passes, with the kstat read around each.
salt_check() {   # salt_check <tree>
	local t=$1 d=$W/A/salt-$1 i h n=0 bad=0 k0 k1 k2 s0
	blocked && return 1
	cd $(tdir $t) || return 1
	mkdir -p $d/vdev $d/mnt && truncate -s 1G $d/vdev/v0 &&
	    dd if=/dev/urandom of=$d/key bs=32 count=1 status=none && chmod 600 $d/key || return 1
	sudo -n ./zpool create -o cachefile=none -m $d/mnt kcsalt $d/vdev/v0 || { stop "$t: kcsalt create"; return 1; }
	s0=$(cat /sys/module/zfs/parameters/zfs_key_max_salt_uses)
	echo 64 | sudo -n tee /sys/module/zfs/parameters/zfs_key_max_salt_uses > /dev/null
	say "zfs_key_max_salt_uses $s0 -> $(cat /sys/module/zfs/parameters/zfs_key_max_salt_uses)"
	sudo -n ./zfs create -o encryption=on -o keyformat=raw -o keylocation=file://$d/key \
	    -o recordsize=128k -o primarycache=metadata -o compression=off -o atime=off kcsalt/enc ||
	    { stop "$t: kcsalt/enc create"; return 1; }
	for i in 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18 19 20; do
		dd if=/dev/urandom of=$d/src bs=128k count=64 status=none
		echo "f$i $(sha256sum < $d/src | cut -d' ' -f1)" >> $PD/salt-$t-written.txt
		sudo -n cp $d/src $d/mnt/enc/f$i && sudo -n ./zpool sync kcsalt || bad=1
	done
	rm -f $d/src
	echo $s0 | sudo -n tee /sys/module/zfs/parameters/zfs_key_max_salt_uses > /dev/null
	same "$t zfs_key_max_salt_uses restored" "$(cat /sys/module/zfs/parameters/zfs_key_max_salt_uses)" $s0 |
	    tee -a $PD/runs.txt
	say "$t written: 20 files of 8 MiB (rc $bad), $(sudo -n ./zfs get -Hp -o value used kcsalt/enc) bytes used"
	sudo -n ./zpool export kcsalt && sudo -n ./zpool import -d $d/vdev -o cachefile=none kcsalt &&
	    sudo -n ./zfs load-key kcsalt/enc > /dev/null && sudo -n ./zfs mount kcsalt/enc ||
	    { stop "$t: kcsalt export, import or key load"; return 1; }
	k0=$(kst)
	for i in 1 2; do
		n=0
		while read -r -u 4 f h; do
			[ "$(sha256sum < $d/mnt/enc/$f | cut -d' ' -f1)" = "$h" ] && n=$((n + 1))
		done 4< $PD/salt-$t-written.txt
		eval k$i=\"$(kst)\"
		say "$t read pass $i: $n of 20 files read back as written"
		[ $n = 20 ] || bad=2
	done
	say "$t kstat (hits misses admissions evictions): after import [$k0], pass 1 [$k1], pass 2 [$k2]"
	if [ "$k0" != absent ]; then
		echo "$k0 $k1 $k2" | awk '{ for (p = 1; p <= 2; p++) printf "pass %d: hits +%d, misses +%d, admissions +%d, evictions +%d\n", p, $(4*p+1) - $(4*p-3), $(4*p+2) - $(4*p-2), $(4*p+3) - $(4*p-1), $(4*p+4) - $(4*p) }' |
		    sed "s/^/$t /" | tee -a $PD/runs.txt
	fi
	sudo -n ./zpool export kcsalt || { held "$t: kcsalt export"; return 1; }
	rm -f $d/vdev/v0
	[ $bad = 2 ] && { stop "$t: a file read back differs from what was written"; return 1; }
	[ $bad = 0 ] || { stop "$t: a write failed"; return 1; }
}

# Part C, cross-version: pool <p> on the sparse file $W/C/xv/<p>/v0 (no
# cache file), its key $W/C/xv/key, the 2 MiB (16 blocks of 128 KiB) file
# $W/C/xv/src that every copy is made of. DEDUP_LOG_TXG_MAX is set to 1 at
# every load (as dedup_encrypted does), so that zdb -D finds every entry in
# the DDT itself; xv_down restores it after the export.
XV=$W/C/xv
xv_up() {   # xv_up <pool>: import, load every key from the key file, mount
	local ds e=0
	cd $(tdir $(cat $L/loaded-tree)) || return 1
	echo 1 | sudo -n tee /sys/module/zfs/parameters/zfs_dedup_log_txg_max > /dev/null
	sudo -n ./zpool import -d $XV/$1 -o cachefile=none $1 || { stop "$1: import"; return 1; }
	for ds in $(sudo -n ./zfs list -H -r -o name,encryptionroot $1 | awk '$1 == $2 { print $1 }'); do
		sudo -n ./zfs load-key -L file://$XV/key $ds > /dev/null || e=1
	done
	sudo -n ./zfs mount -a || e=1
	[ $e = 0 ] || { stop "$1: key load or mount"; return 1; }
	say "  $1 imported: $(sudo -n ./zfs list -H -r -o name,keystatus,mounted $1 | awk '{ printf "%s%s %s %s", s, $1, $2, $3; s = "; " }')"
}

# Receive the raw stream <file> as <dataset>, load its key from the key file
# (its keylocation stays "prompt"), and mount it.
xv_recv() {   # xv_recv <file> <dataset>
	sudo -n ./zfs receive $2 < $1 &&
	    sudo -n ./zfs load-key -L file://$XV/key $2 > /dev/null && sudo -n ./zfs mount $2 &&
	    say "  $2: received raw from $(basename $1), key loaded, mounted" ||
	    { say "  $2: raw receive, key load or mount FAILED"; touch $PD/XVBAD; }
}

# The sha256 of each copy against src's: "<dataset>/<file> ok" lines.
xv_read() {   # xv_read <pool> <dataset/file>...
	local p=$1 x h s
	s=$(sha256sum < $XV/src | cut -d' ' -f1)
	shift
	for x in "$@"; do
		h=$(sha256sum < $XV/mnt-$p/${x#$p/} 2>&1 | cut -d' ' -f1)
		[ "$h" = "$s" ] && say "  $x: read back as src" || { say "  $x: DIFFERS ($h)"; touch $PD/XVBAD; }
	done
}

xv_copy() {   # xv_copy <pool> <dataset/file>: write a copy of src
	sudo -n cp $XV/src $XV/mnt-$1/${2#$1/} && say "  $2: written" || { say "  $2: write FAILED"; touch $PD/XVBAD; }
}

xv_scrub() {   # xv_scrub <pool>
	local s
	sudo -n ./zpool scrub -w $1
	s=$(sudo -n ./zpool status $1)
	say "  scrub $1: $(echo "$s" | sed -n 's/^ *scan: //p'); $(echo "$s" | sed -n 's/^errors: //p')"
	echo "$s" | grep -q '^errors: No known data errors$' && echo "$s" | grep -q 'with 0 errors' ||
	    touch $PD/XVBAD
}

# Export, then with the loaded tree's zdb: the DDT's duplicate, unique and
# log entries, the keys recording DSL_CRYPTO_DEDUP_PRF = 1, and zdb -bcc;
# each against <dup> <uniq> <prf>.
xv_down() {   # xv_down <pool> <label> <dup> <uniq> <prf>
	local p=$1 d u lg k b
	sudo -n ./zpool sync $p && sudo -n ./zpool sync $p && sudo -n ./zpool export $p ||
	    { held "$p: export"; return 1; }
	cat $L/tunables-loaded.txt | tr ' ' '\n' | sed -n 's/^zfs_dedup_log_txg_max=//p' |
	    sudo -n tee /sys/module/zfs/parameters/zfs_dedup_log_txg_max > /dev/null
	sudo -n ./zdb -e -p $XV/$p -D $p > $PD/xv-$2-D.txt 2>&1
	d=$(awk -F'entries=' '/^DDT-sha256-zap-duplicate:/ { print $2 + 0 }' $PD/xv-$2-D.txt)
	u=$(awk -F'entries=' '/^DDT-sha256-zap-unique:/ { print $2 + 0 }' $PD/xv-$2-D.txt)
	lg=$(awk -F'entries=' '/^DDT-log-sha256-/ { n += $2 } END { print n + 0 }' $PD/xv-$2-D.txt)
	k=$(sudo -n ./zdb -e -p $XV/$p -dddd $p 2>&1 | grep -cE 'DSL_CRYPTO_DEDUP_PRF = 1 ?$')
	sudo -n ./zdb -e -p $XV/$p -bcc $p > $PD/xv-$2-bcc.txt 2>&1
	b=$?
	say "  $2: DDT duplicate ${d:-0} (expected $3), unique ${u:-0} (expected $4), log $lg (expected 0);" \
	    "keys recording DSL_CRYPTO_DEDUP_PRF = 1: $k (expected $5); zdb -bcc rc=$b," \
	    "$(grep -c 'No leaks' $PD/xv-$2-bcc.txt) 'No leaks'"
	[ "${d:-0} ${u:-0} $lg $k $b" = "$3 $4 0 $5 0" ] || touch $PD/XVBAD
}

xv_check() {   # xv_check <label>: stop the part if anything differed
	if [ -e $PD/XVBAD ]; then
		mv $PD/XVBAD $PD/XVBAD-$1
		stop "$1: cross-version result not as expected"
		return 1
	fi
	say "$1: as expected"
}

# Part B. One fio random-read job of <runtime> s over the 16 files of dir
# <name> (old or dd), 16 jobs, as one line.
fio_cmd() {   # fio_cmd <name> <runtime> <json>
	echo "nice -n 10 ionice -c3 $FIO --name=$1 --directory=$W/B/mnt/enc/$1 --rw=randread --bs=4k" \
	    "--size=64m --numjobs=16 --iodepth=32 --ioengine=libaio --time_based --runtime=$2" \
	    "--group_reporting --allow_file_create=0 --output-format=json --output=$3"
}

fio_json() {   # fio_json <json>: IOPS, reads, mean latency (us), error
	python3 -I -c 'import json, sys
j = json.load(open(sys.argv[1]))["jobs"][0]
print("%d %d %.1f %d" % (round(j["read"]["iops"]), j["read"]["total_ios"],
      j["read"]["lat_ns"]["mean"] / 1000, j["error"]))' "$1" 2>/dev/null || echo "0 0 0 1"
}

cpu_pct() {   # cpu_pct "<cpu line before>" "<after>": %sys %usr %steal
	echo "$1 $2" | awk '{ for (i = 2; i <= 9; i++) { d[i] = $(i + 11) - $i; t += d[i] }
	    printf "%.1f %.1f %.1f", 100 * d[4] / t, 100 * (d[2] + d[3]) / t, 100 * d[9] / t }'
}

# One measured phase on the loaded release tree: old alone, dd alone, or
# both at once (two fio processes, 32 jobs), with the CPU counters, the
# page-ins and the kstat around it. One row per fio job set.
perf_phase() {   # perf_phase <round> <tree> <old|dd|mix>
	local r=$1 t=$2 ph=$3 s0 s1 g0 g1 k0 k1 c x p q rc=0 n
	s0=$(grep '^cpu ' /proc/stat); g0=$(awk '$1 == "pgpgin" { print $2 }' /proc/vmstat); k0=$(kst)
	if [ $ph = mix ]; then
		$(fio_cmd old $RT $PD/fio-$r-$t-mix-old.json) < /dev/null > $PD/fio-$r-$t-mix-old.err 2>&1 &
		p=$!
		$(fio_cmd dd $RT $PD/fio-$r-$t-mix-dd.json) < /dev/null > $PD/fio-$r-$t-mix-dd.err 2>&1 || rc=1
		wait $p || rc=1
	else
		$(fio_cmd $ph $RT $PD/fio-$r-$t-$ph.json) < /dev/null > $PD/fio-$r-$t-$ph.err 2>&1 || rc=1
	fi
	s1=$(grep '^cpu ' /proc/stat); g1=$(awk '$1 == "pgpgin" { print $2 }' /proc/vmstat); k1=$(kst)
	c=$(cpu_pct "$s0" "$s1")
	if [ "$k0" = absent ]; then
		x="n/a n/a n/a n/a"
	else
		x=$(echo "$k0 $k1" | awk '{ print $5 - $1, $6 - $2, $7 - $3, $8 - $4 }')
	fi
	for q in $([ $ph = mix ] && echo mix-old mix-dd || echo $ph); do
		n=$(fio_json $PD/fio-$r-$t-$q.json)
		set -- $n
		[ "$4" = 0 ] || rc=1
		say "$r $t $q IOPS $1 lat ${3}us sys/usr/steal ${c// /\/}% pgpgin $((g1 - g0))KiB kstat +[$x]"
		printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $r $t $q $1 $3 "$c" $((g1 - g0)) "$x" >> $PD/results.tsv
	done
	[ $rc = 0 ] || { stop "$r $t $ph: fio failed"; return 1; }
}

# Import, load the key (a new current salt), mount, and check what every
# load must find: the pool, the key, 16 + 16 files of 64 MiB, parameters.
perf_up() {   # perf_up <label>
	local x e=
	cd $(tdir $(cat $L/loaded-tree)) || return 1
	sudo -n ./zpool import -d $W/B/vdev -o cachefile=none kcperf || e="$e import"
	sudo -n ./zfs load-key kcperf/enc > /dev/null || e="$e load-key"
	sudo -n ./zfs mount kcperf/enc || e="$e mount"
	x=$(sudo -n ./zfs get -H -o value keystatus,mounted,dedup kcperf/enc | xargs)
	[ "$x" = "available yes on" ] || e="$e key/mount/dedup($x)"
	x=$(stat -c %s $W/B/mnt/enc/old/old.*.0 $W/B/mnt/enc/dd/dd.*.0 2>&1 | sort | uniq -c | xargs)
	[ "$x" = "32 67108864" ] || e="$e files($x)"
	for x in $PP; do echo "$x=$(cat /sys/module/zfs/parameters/$x 2>&1)"; done > $PD/params-$1.txt
	[ -s $PD/params-first.txt ] || cp $PD/params-$1.txt $PD/params-first.txt
	cmp -s $PD/params-first.txt $PD/params-$1.txt || e="$e parameters"
	[ -z "$e" ] && say "ok: $1 up" || { stop "$1 up:$e"; return 1; }
}

perf_down() {   # perf_down <label>
	sudo -n ./zpool export kcperf || { held "$1: export"; return 1; }
	[ -z "$(sudo -n ./zpool list -H -o name)$(others)" ] || { held "$1: pool or process left"; return 1; }
	say "ok: $1 down"
}

# sha256 of each directory's 16 files, in order, read back on the loaded
# tree; after "prep", against what M-rel read at the preparation.
perf_hash() {   # perf_hash <label>
	local x
	for x in old dd; do
		cat $(for i in 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do printf '%s ' $W/B/mnt/enc/$x/$x.$i.0; done) |
		    sha256sum | cut -d' ' -f1 > $PD/hash-$x-$1.txt
		[ $1 = prep ] && continue
		same "$1 $x read back as M-rel read it at the preparation" "$(cat $PD/hash-$x-$1.txt)" \
		    "$(cat $PD/hash-$x-prep.txt)" | tee -a $PD/runs.txt
	done
}

# Part D: one ztest run of <tree>'s userspace build (run as this user, no
# core files), with <seed> if given, then its summary. ztest keeps zdb's
# output to itself: when its final zdb fails, the same zdb command runs
# again on the pool ztest left, and its ERROR lines are kept.
ztest_run() {   # ztest_run <label> <tree> [seed]
	local d=$W/D/$1 td=$(tdir $2) rc t0 z
	blocked && return 1
	[ "$(df -BG --output=avail /var/tmp | tail -1 | tr -dc 0-9)" -ge 5 ] || { stop "$1: under 5G free"; return 1; }
	mkdir -p $d || return 1
	t0=$(date +%s)
	(cd $d && ulimit -c 0 && LD_LIBRARY_PATH=$td/.libs timeout -k 60 1500 nice -n 10 ionice -c3 \
	    $td/.libs/ztest -V -T 300 ${3:+-J $3} -f $d) < /dev/null > $PD/$1.log 2>&1
	rc=$?
	tr '\r' '\n' < $PD/$1.log > $PD/$1.txt
	z=$(sed -n "s/^ztest: '\(.*zdb .*\)' exit code [0-9]*$/\1/p" $PD/$1.txt | tail -n 1)
	say "$1 $2${3:+ seed ${3:0:16}...}: rc=$rc wall=$(( $(date +%s) - t0 ))s;" \
	    "$(grep -m 1 'vdevs, ' $PD/$1.txt); $(grep -c '^Pass ' $PD/$1.txt) passes" \
	    "($(grep -c '^Pass .*SIGKILL' $PD/$1.txt) SIGKILLed); seed $(sed -n 's/^primary_prng_seed=//p' $PD/$1.txt | cut -c1-16)...;" \
	    "final zdb: $([ -n "$z" ] && echo failed || echo 'no failure reported')"
	grep -E 'exit code|no pools found|^ztest: |VERIFY|ASSERT|[Ll]ivelist' $PD/$1.txt | head -n 6 | sed 's/^/  /' |
	    tee -a $PD/runs.txt
	if [ -n "$z" ]; then
		timeout -k 60 900 nice -n 10 ionice -c3 $z < /dev/null > $PD/$1-zdb.txt 2>&1
		say "  zdb again rc=$?: ERROR lines $(grep -c '^ERROR' $PD/$1-zdb.txt), livelist lines $(grep -ci livelist $PD/$1-zdb.txt)"
		grep -E '^ERROR|[Ll]ivelist' $PD/$1-zdb.txt | head -n 8 | sed 's/^/    /' | tee -a $PD/runs.txt
	fi
	echo "$1 $2 rc=$rc zdb=$([ -n "$z" ] && echo failed || echo ok)" >> $PD/ztest-results.txt
	rm -rf $d
	[ $rc -eq 124 ] || [ $rc -eq 137 ] && { stop "$1: outer timeout"; return 1; }
	return 0
}
ENV
cat > $L/count.py <<'PY'
import configparser, os, sys
# count.py <tree> <tags,...> | -t <dir/test>: the Test: lines one
# zfs-tests.sh run prints, read the way test-runner.py reads the runfiles.
tree = sys.argv[1]
suite = os.path.join(tree, 'tests/zfs-tests')
rf = [os.path.join(tree, 'tests/runfiles', f) for f in ('common.run', 'linux.run')]
c = configparser.RawConfigParser()
c.read(rf)
if sys.argv[2] == '-t':
    # -t runs the test alone, with its directory's setup and cleanup when
    # they are executable (zfs-tests.sh), whatever the runfile says.
    d, t = os.path.split(sys.argv[3])
    p = os.path.join(suite, 'tests/functional', d)
    ok = os.access(os.path.join(p, t + '.ksh'), os.X_OK)
    n = sum(os.access(os.path.join(p, s + '.ksh'), os.X_OK)
            for s in ('setup', 'cleanup'))
    listed = [s for s in c.sections() if s.split(':', 1)[0] ==
              'tests/functional/' + d and t in eval(c.get(s, 'tests'))]
    print('%s %d%s%s' % (sys.argv[3], n + 1, '' if ok else ' MISSING',
          '' if listed else ' UNLISTED'))
    sys.exit(0)
tags = set(sys.argv[2].split(','))
total = 0
for s in c.sections():
    if 'tests' not in c.options(s) or not tags & set(eval(c.get(s, 'tags'))):
        continue
    d = os.path.join(suite, s.split(':', 1)[0])
    tests = eval(c.get(s, 'tests'))
    miss = [t for t in tests if not os.access(os.path.join(d, t + '.ksh'), os.X_OK)]
    aux = sum(1 for k in ('pre', 'post') if c.get(s, k).strip())
    total += len(tests) + aux
    print('  %s: %d tests + %d setup/cleanup%s' % (s, len(tests), aux,
          ' MISSING ' + ' '.join(miss) if miss else ''))
print('%s %d' % (sys.argv[2], total))
PY
cat > $L/table.py <<'PY'
import statistics, sys
# table.py <results.tsv> <runtime s>: part B's table, median [min-max] of
# each cell over the rounds. Row: round tree phase iops lat_us
# "sys usr steal" pgpgin "hits misses admissions evictions" (n/a on M-rel).
rt = float(sys.argv[2])
cells = {}
for line in open(sys.argv[1]):
    f = line.rstrip('\n').split('\t')
    if len(f) != 8:
        continue
    cpu = f[5].split()
    ks = f[7].split()
    cells.setdefault((f[2], f[1]), []).append(dict(
        rnd=f[0], iops=float(f[3]), sys=float(cpu[0]), steal=float(cpu[2]),
        pg=float(f[6]), ks=None if ks[0] == 'n/a' else [float(x) for x in ks]))


def mr(v, fmt='%.0f'):
    return (fmt + ' [' + fmt + '-' + fmt + ']') % (statistics.median(v), min(v), max(v))


def med(phase, tree):
    c = cells.get((phase, tree), [])
    return statistics.median([x['iops'] for x in c]) if c else None


phases = ('old', 'dd', 'mix-old', 'mix-dd')
print('%-8s %-7s %-2s %-26s %-20s %-7s %-9s %s' % (
    'phase', 'tree', 'n', 'IOPS', '%sys', '%steal', 'pgpgin', 'kstat per s: hits misses; admissions evictions'))
for ph in phases:
    for tree in ('M-rel', 'KC-rel'):
        c = cells.get((ph, tree), [])
        if not c:
            print('%-8s %-7s none' % (ph, tree))
            continue
        k = [x['ks'] for x in c if x['ks'] is not None]
        ktxt = 'n/a'
        if k:
            ktxt = '%.0f %.0f; %.0f %.0f' % tuple(
                statistics.median([x[i] for x in k]) / (rt if i < 2 else 1) for i in range(4))
        print('%-8s %-7s %-2d %-26s %-20s %-7s %-9s %s' % (
            ph, tree, len(c), mr([x['iops'] for x in c]), mr([x['sys'] for x in c], '%.1f'),
            '%.1f' % max(x['steal'] for x in c), '%.0f' % max(x['pg'] for x in c), ktxt))
    m, b = med(ph, 'M-rel'), med(ph, 'KC-rel')
    if m and b:
        print('%-8s KC/M    IOPS %+.1f%% (medians)' % (ph, 100 * (b / m - 1)))
for tree in ('M-rel', 'KC-rel'):
    o, mo, d, md = (med(p, tree) for p in phases)
    if o and mo and d and md:
        print('%-7s beside the other job: old keeps %.0f%% of its IOPS alone, dd %.0f%%' % (
            tree, 100 * mo / o, 100 * md / d))
print('per round (IOPS M-rel/KC-rel): ' + '; '.join(
    '%s %s: %s' % (ph, x['rnd'], '/'.join(
        '%.0f' % y['iops'] for t in ('M-rel', 'KC-rel')
        for y in cells.get((ph, t), []) if y['rnd'] == x['rnd']))
    for ph in phases for x in cells.get((ph, 'M-rel'), [])))
PY
cat > $L/listA.txt <<'LIST'
# label how argument; how is -T (tags) or -t (a test, as dir/test under
# tests/functional, run alone with its directory's setup and cleanup)
kg  -T zfs_load-key,zfs_unload-key,zfs_change-key
r01 -t rsend/recv_dedup_encrypted_zvol
r02 -t rsend/send-wR_encrypted_zvol
r03 -t rsend/send_encrypted_files
r04 -t rsend/send_encrypted_freeobjects
r05 -t rsend/send_encrypted_hierarchy
r06 -t rsend/send_encrypted_holds
r07 -t rsend/send_encrypted_incremental
r08 -t rsend/send_encrypted_props
r09 -t rsend/send_encrypted_truncated_files
r10 -t rsend/send_leak_keymaps
r11 -t rsend/send_mixed_raw
r12 -t rsend/send_raw_ashift
r13 -t rsend/send_raw_large_blocks
r14 -t rsend/send_raw_spill_block
r15 -t rsend/send_realloc_encrypted_files
c01 -t cli_root/zfs_receive/zfs_receive_from_encrypted
c02 -t cli_root/zfs_receive/zfs_receive_to_encrypted
c03 -t cli_root/zfs_receive/zfs_receive_raw
c04 -t cli_root/zfs_receive/zfs_receive_raw_incremental
c05 -t cli_root/zfs_receive/zfs_receive_raw_-d
c06 -t cli_root/zfs_receive/zfs_receive_raw_-b
c07 -t cli_root/zfs_receive/zfs_receive_-wR-encrypted-mix
c08 -t cli_root/zfs_receive/zfs_receive_corrective
c09 -t cli_root/zfs_receive/zfs_receive_compressed_corrective
c10 -t cli_root/zfs_receive/zfs_receive_large_block_corrective
l2  -t l2arc/persist_l2arc_002_pos
s1  -t cli_root/zpool_scrub/zpool_scrub_encrypted_unloaded
s2  -t cli_root/zpool_scrub/zpool_scrub_thorough
i1  -t cli_root/zpool_import/zpool_import_encrypted
i2  -t cli_root/zpool_import/zpool_import_encrypted_load
i3  -t cli_root/zpool_import/zpool_import_encrypted_zplprops
i4  -t cli_root/zpool_import/zpool_import_errata3
i5  -t cli_root/zpool_import/zpool_import_errata4
k1  -t block_cloning/block_cloning_cross_enc_dataset
k2  -t block_cloning/block_cloning_replay_encrypted
z1  -t cli_root/zdb/zdb_encrypted
z2  -t cli_root/zdb/zdb_encrypted_raw
xg  -T send_xdr_encoding
sg  -T zfs_send
cr  -T crypto
dg  -T dedup
LIST
cat > $L/listC.txt <<'LIST'
# label how argument; labels shared with part A's list name the same run
de  -t dedup/dedup_encrypted
dg  -T dedup
kg  -T zfs_load-key,zfs_unload-key,zfs_change-key
p1  -t cli_root/zpool_create/zpool_create_crypt_combos
p2  -t cli_root/zpool_create/zpool_create_encrypted
f1  -t cli_root/zfs_create/zfs_create_crypt_combos
f2  -t cli_root/zfs_create/zfs_create_encrypted
r01 -t rsend/recv_dedup_encrypted_zvol
r02 -t rsend/send-wR_encrypted_zvol
r03 -t rsend/send_encrypted_files
r04 -t rsend/send_encrypted_freeobjects
r05 -t rsend/send_encrypted_hierarchy
r06 -t rsend/send_encrypted_holds
r07 -t rsend/send_encrypted_incremental
r08 -t rsend/send_encrypted_props
r09 -t rsend/send_encrypted_truncated_files
r10 -t rsend/send_leak_keymaps
r11 -t rsend/send_mixed_raw
r12 -t rsend/send_raw_ashift
r13 -t rsend/send_raw_large_blocks
r14 -t rsend/send_raw_spill_block
r15 -t rsend/send_realloc_encrypted_files
c01 -t cli_root/zfs_receive/zfs_receive_from_encrypted
c02 -t cli_root/zfs_receive/zfs_receive_to_encrypted
c03 -t cli_root/zfs_receive/zfs_receive_raw
c04 -t cli_root/zfs_receive/zfs_receive_raw_incremental
c05 -t cli_root/zfs_receive/zfs_receive_raw_-d
c06 -t cli_root/zfs_receive/zfs_receive_raw_-b
c07 -t cli_root/zfs_receive/zfs_receive_-wR-encrypted-mix
i1  -t cli_root/zpool_import/zpool_import_encrypted
i2  -t cli_root/zpool_import/zpool_import_encrypted_load
i3  -t cli_root/zpool_import/zpool_import_encrypted_zplprops
i4  -t cli_root/zpool_import/zpool_import_errata3
i5  -t cli_root/zpool_import/zpool_import_errata4
z1  -t cli_root/zdb/zdb_encrypted
z2  -t cli_root/zdb/zdb_encrypted_raw
LIST
. $L/env.sh
bash -n $L/env.sh && echo "env.sh syntax ok"
mkdir $L/A $L/B $L/C $L/D $W/A $W/B $W/C $W/D && mkdir -m 1777 $W/files-A $W/files-C &&
    echo "directories made: files-A $(stat -c %a $W/files-A), files-C $(stat -c %a $W/files-C)"
for f in env.sh count.py table.py listA.txt listC.txt; do
	echo "$f: sha256 $(sha256sum < $L/$f | cut -d' ' -f1)," \
	    "without blanks $(tr -d ' \t' < $L/$f | sha256sum | cut -c1-16)"
done | tee $L/files.txt
fi
```

Expected: `env.sh syntax ok`, `directories made: files-A 1777, files-C 1777`,
then

```
env.sh: sha256 49d7112301845866d809d0d281cea95147e8e1f942198d8419cdcc012520691e, without blanks 5c1e97b7acbbd4d4
count.py: sha256 7a3f1e11c77074e5c8a683ae7c4d85a07e2f1d04f0895d598f6adaed6fc721f8, without blanks a201f867df14f5c9
table.py: sha256 67a9723c5b9ed03e3098657aee1f712684947e04c972ee30d75aa9457ccfe747, without blanks 28a0edf5ae3f26b8
listA.txt: sha256 4954df4034154ee1488502df04667007f7be37759967eae5e266784defc3345d, without blanks b8f75569f5a15f77
listC.txt: sha256 872894a5d24407b2de4c62f188b61ba91c31347131253a59a9d4ff6dd70e9afd, without blanks d71ba06369895b36
```

A file whose full hash differs but whose hash without blanks matches lost or
gained only spaces or tabs in the copy: fine for `env.sh` and the lists, a
stop for the two Python files (Python reads indentation). A file whose hash
without blanks differs was copied wrongly: stop.

Then check the five trees as built, record each one's srcversion and module
hashes for the guard, and the pid file path of the trees' `zed`:

```sh
. /var/log/zts-enc-1026/env.sh; part A
CKC="93f2c3c24ba125175ea3b56337caaf77562c5859 10b18aa236e01b9075e4a08ec41091609d18c1f4 d511c2638b3e0a20e4a039932150a0d5e24a3027"
CIV="e5a8ea74264f034bf372bd66e70cf0d5e96ccfc3 990286efdb9971bbe68b1e9ff73d7165f371b3e6 6fcd458a731aa2be6d012616747ccbde0bf11f6f"
DE=tests/zfs-tests/tests/functional/dedup/dedup_encrypted.ksh
{
cd $W/KC
same "KC head" "$(git rev-parse HEAD)" $HKC
same "KC's commits on master" "$(git rev-list --reverse $HM..HEAD | xargs)" "$CKC"
same "KC against master" "$(git diff --stat $HM HEAD | tail -1)" " 7 files changed, 457 insertions(+), 19 deletions(-)"
same "KC files under tests/" "$(git diff --name-only $HM HEAD -- tests | wc -l)" 0
cd $W/IV
same "IV head" "$(git rev-parse HEAD)" $HIV
same "IV's commits on master" "$(git rev-list --reverse $HM..HEAD | xargs)" "$CIV"
same "IV against master" "$(git diff --stat $HM HEAD | tail -1)" " 16 files changed, 754 insertions(+), 21 deletions(-)"
same "IV's dedup_encrypted.ksh" "$(git hash-object $DE) $(stat -c %a $DE)" "39bfb2005a247fbc83863b90658f2bdee4228549 755"
same "KC-rel head" "$(git -C $W/KC-rel rev-parse HEAD)" $HKC
same "M-rel head" "$(git -C $W/M-rel rev-parse HEAD)" $HM
same "M head" "$(git -C $MT rev-parse HEAD)" $HM
for t in KC IV KC-rel M-rel M; do
	cd $(tdir $t)
	case $t in KC*) s=$SRCV_KC ;; IV) s=$SRCV_IV ;; *) s=$SRCV_M ;; esac
	same "$t tracked changes" "$(git status --short --untracked-files=no | wc -l)" 0
	same "$t ZFS_DEBUG lines" "$(grep -c '^#define ZFS_DEBUG 1$' zfs_config.h)" "$([ $(kind $t) = dbg ] && echo 3 || echo 1)"
	same "$t backend: ICP (no kcapi)" "$(grep -c '^#define HAVE_KERNEL_CRYPTO 1$' zfs_config.h)" 0
	same "$t srcversion" "$(modinfo -F srcversion module/zfs.ko)" $s
	modinfo -F srcversion module/zfs.ko > $L/srcv-$t
	sha256sum module/zfs.ko module/spl.ko > $L/ko-$t.sha256
	grep -aoE '/[[:alnum:]/._-]+/zed[.]pid' .libs/zed 2>/dev/null | head -n 1 >> $L/zedpid.txt
	if [ -s $L/build-$t.log ]; then
		echo "$t build log: $(grep -cE ': error:|\*\*\* \[' $L/build-$t.log) errors," \
		    "$(grep -cE ':[0-9]+:[0-9]+: warning:' $L/build-$t.log) compiler warnings," \
		    "$(grep -c 'warning: objtool:' $L/build-$t.log) objtool notes"
	else
		echo "$t build log: none in $L (recorded)"
	fi
done
sort -u -o $L/zedpid.txt $L/zedpid.txt
same "KC: cache code and kstat" "$(grep -c 'zio_crypt_salt_key_hold(' $W/KC/module/zfs/zio_crypt.c) $(grep -c '"zio_crypt_stats"' $W/KC/module/zfs/zio_crypt.c)" "2 1"
same "M: neither" "$(grep -c 'zio_crypt_salt_key_hold(' $MT/module/zfs/zio_crypt.c) $(grep -c '"zio_crypt_stats"' $MT/module/zfs/zio_crypt.c)" "0 0"
same "IV, KC and M: the IV tunable" "$(grep -c 'crypt_dedup_blake3, INT' $W/IV/module/zfs/dsl_crypt.c) $(grep -c 'crypt_dedup_blake3' $W/KC/module/zfs/dsl_crypt.c) $(grep -c 'crypt_dedup_blake3' $MT/module/zfs/dsl_crypt.c)" "1 0 0"
echo "zed pid file: $(xargs < $L/zedpid.txt)"
for t in KC IV M; do
	echo "$t: $(grep -E '^#define LIBFETCH_IS_(FETCH|LIBCURL) ' $(tdir $t)/zfs_config.h | cut -d' ' -f2- | xargs)"
done
} 2>&1 | tee $L/checks.txt
echo "$(grep -c '^ok:' $L/checks.txt) ok, $(grep -c '^MISMATCH' $L/checks.txt) mismatch"
```

Expected: `ok:` lines only, and last `34 ok, 0 mismatch`. Build logs: `KC
build log: 0 errors, 0 compiler warnings, <n> objtool notes` and the same for
IV (any other count of errors or compiler warnings is a stop); for the other
trees, the counts or `none in $L` (recorded: the master tree's logs live with
its own task). `zed pid file: /usr/local/var/run/zed.pid` (recorded: no
selected test starts a `zed`), and the `LIBFETCH_IS_*` values of each tree
(recorded: both 0 means libzfs has no https fetcher and `zfs_load-key_https`
fails on every tree). Any `MISMATCH` means stop.

Then the lists against each tree's own runfiles, and the runners' results
directories (KC and M to part A's, IV to part C's; STEP 7 points M at part
C's):

```sh
. /var/log/zts-enc-1026/env.sh; part A
for x in A:KC A:M C:IV C:M; do
	p=${x%%:*}; t=${x#*:}
	while read -r -u 3 label how arg; do
		case $label in '#'*|'') continue ;; esac
		if [ $how = -T ]; then
			python3 -I $L/count.py $(tdir $t) $arg > $L/$p/sections-$t-$label.txt
			echo "$label $(tail -1 $L/$p/sections-$t-$label.txt | cut -d' ' -f2)$(grep -q ' MISSING ' $L/$p/sections-$t-$label.txt && echo ' MISSING')"
		else
			echo "$label $(python3 -I $L/count.py $(tdir $t) -t $arg | cut -d' ' -f2-)"
		fi
	done 3< $L/list$p.txt > $L/$p/n-$t.txt
	echo "list $p on $t: $(grep -c . $L/$p/n-$t.txt) runs, $(awk '{ s += $2 } END { print s }' $L/$p/n-$t.txt) result lines;" \
	    "flags: [$(awk 'NF > 2 { $2 = ""; printf "%s%s", s, $0; s = "; " }' $L/$p/n-$t.txt)]"
done | tee -a $L/checks.txt
set_basedir KC $L/A/results | tee -a $L/checks.txt
set_basedir M $L/A/results | tee -a $L/checks.txt
set_basedir IV $L/C/results | tee -a $L/checks.txt
grep -h dedup $L/A/sections-KC-dg.txt $L/C/sections-IV-dg.txt
```

Expected:

```
list A on KC: 42 runs, 185 result lines; flags: []
list A on M: 42 runs, 185 result lines; flags: []
list C on IV: 36 runs, 144 result lines; flags: []
list C on M: 36 runs, 143 result lines; flags: [de  MISSING UNLISTED]
ok: KC results directory /var/log/zts-enc-1026/A/results
ok: M results directory /var/log/zts-enc-1026/A/results
ok: IV results directory /var/log/zts-enc-1026/C/results
  tests/functional/dedup: 16 tests + 0 setup/cleanup
  tests/functional/dedup:Linux: 1 tests + 2 setup/cleanup
dedup 19
  tests/functional/dedup: 17 tests + 0 setup/cleanup
  tests/functional/dedup:Linux: 1 tests + 2 setup/cleanup
dedup 20
```

`de` is missing from M until STEP 7 copies it in (its 3 lines are its
directory's setup, the test and the cleanup). Any other count or flag, or a
`MISMATCH`, means stop.

## STEP 2: baselines

```sh
. /var/log/zts-enc-1026/env.sh; part A
{
date '+%F %T' > $L/start.txt
loops > $L/loops-before.txt
sudo -n dmsetup ls > $L/dm-before.txt 2>&1
ls -A / > $L/root-before.txt
sudo -n journalctl -k -b --no-pager -q | kfilter > $L/journal-boot.txt
echo "baseline at $(cat $L/start.txt): loops [$(xargs < $L/loops-before.txt)], dm [$(cat $L/dm-before.txt)]," \
    "journal (this boot) $(grep -ciE "$KRE" $L/journal-boot.txt) suspicious lines," \
    "$(grep -c '^DEPOT SPLAT' $L/journal-boot.txt) depot splats; / holds $(wc -l < $L/root-before.txt) entries;" \
    "zpool.cache $(stat -c '%s bytes' /etc/zfs/zpool.cache 2>/dev/null || echo absent)"
} | tee $L/baseline.txt
```

Expected: one `baseline at ...` line with `loops []` and `dm [No devices
found]`; the counts are recorded only.

## STEP 3: part A on KC: the salt check, then list A

```sh
. /var/log/zts-enc-1026/env.sh; part A
if ! blocked && switch_to KC; then
	since=$(date '+%F %T')
	salt_check KC
	kern_check salt-KC "$since"
	run_list kc KC $L/listA.txt
fi
step_end 3
```

Expected first:

```
load KC rc=0
KC guard ok: srcversion 3B1A49E4C8B4EAFE06AC02A, zfs_flags 4294932990, coresize <n>, kstat yes, IV tunable no
zfs_key_max_salt_uses 400000000 -> 64
ok: KC zfs_key_max_salt_uses restored
KC written: 20 files of 8 MiB (rc 0), <about 170000000> bytes used
KC read pass 1: 20 of 20 files read back as written
KC read pass 2: 20 of 20 files read back as written
KC kstat (hits misses admissions evictions): after import [<4 numbers>], pass 1 [...], pass 2 [...]
KC pass 1: hits +<h1>, misses +<m1>, admissions +<a1>, evictions +<e1>
KC pass 2: hits +<h2>, misses +<m2>, admissions +<a2>, evictions +<e2>
kernel salt-KC: 0 findings, 0 ZTS marks
```

with, in each pass, hits at least 1000, misses between 20 and 400 (two per
salt read, plus a few concurrent first misses; master would derive a key for
each of the 1280 reads), admissions at least 16 and evictions at least 1
(recorded: outside these the cache did not work as designed, which the
summary reports, but it is not a stop). A file that does not read back as
written is a stop (`a file read back differs`): it is the most important
result of the part. Then, for each of the 42 runs, in list order:

```
kc-kg KC -T zfs_load-key,zfs_unload-key,zfs_change-key PASS x26 (26 of 26 lines) rc=0 wall=<s>s
kernel kc-kg: 0 findings, <n> ZTS marks
  zpool.cache: <0 bytes or absent>
leftovers after kc-kg: none
kc-r01 KC -t rsend/recv_dedup_encrypted_zvol PASS x3 (3 of 3 lines) rc=0 wall=<s>s
...
kc-dg KC -T dedup PASS x19 (19 of 19 lines) rc=0 wall=<s>s
```

each followed by its kernel, `zpool.cache` and leftovers lines (a `master's
known test leaks, removed:` line is recorded, not a stop), then `list
listA.txt done on KC` and `STEP 3 done <time>`. Every result is `PASS x<n>`
with `n` the label's row in STEP 1 (3 for each `-t` run but `z1`, `z2`: 1;
`kg` 26, `xg` 16, `sg` 15, `cr` 2, `dg` 19). A test that does not pass is
reported by its own lines (its result line, `note:` lines, the first failing
line, the first `ERROR:` line) and the list goes on. Rough wall times: `kg`
1-3 min, each `r` and `c` run 0.5-2 min (`r15` and `c08`-`c10` up to 4), `l2`
1-3, `s2` 2-5, `dg` 3-6, the others under a minute; 30 to 45 minutes in all.

## STEP 4: part A on M: the same salt check (no kstat), then list A

```sh
. /var/log/zts-enc-1026/env.sh; part A
if ! blocked && switch_to M; then
	since=$(date '+%F %T')
	salt_check M
	kern_check salt-M "$since"
	run_list m M $L/listA.txt
fi
step_end 4
```

Expected: `unload KC rc=0`, `load M rc=0`, `M guard ok: srcversion
8852710DB871A7587B9E722, zfs_flags 4294932990, coresize <n>, kstat no, IV
tunable no`, the salt lines with `M` (both passes `20 of 20`, and `M kstat
...: after import [absent], pass 1 [absent], pass 2 [absent]`, no delta
line), `kernel salt-M: 0 findings`, the 42 runs labelled `m-...` with the
same results as on KC, `list listA.txt done on M`, `STEP 4 done <time>`.
30 to 45 minutes.

## STEP 5: part A: KC-only failures once more, and KC against M

```sh
. /var/log/zts-enc-1026/env.sh; part A
compare_zts kc $L/listA.txt > $PD/compare-first.txt
if grep -qE 'BRANCH ONLY: .* (FAIL|KILLED)$' $PD/compare-first.txt && ! blocked; then
	switch_to KC && rerun_branch_only kc KC $L/listA.txt
fi
{
sudo -n journalctl -k --since "$(cat $L/start.txt)" --no-pager -q -o short-iso 2>&1 | kfilter > $PD/kernf-part.txt
echo "kernel, part A: $(grep -ciE "$KRE" $PD/kernf-part.txt) findings"
for p in kc m kcr; do
	echo "$p: $(cat $PD/$p-*.log 2>/dev/null | grep -E "$TRE" | tally) in $(grep -c "^$p-" $PD/zts-results.txt) runs"
done
sort -u $PD/missing-utils.txt
compare_zts kc $L/listA.txt
grep '^kcr-' $PD/zts-results.txt | sed 's/^/rerun on KC: /'
grep -h 'pass [12]: hits' $PD/runs.txt
echo "flags: STOP [$(cat $PD/STOP 2>/dev/null)] HELD [$(ls $L/HELD 2>/dev/null)] PANIC [$(ls $L/PANIC 2>/dev/null)]"
} 2>&1 | tee $PD/compare.txt
step_end 5
```

Expected:

```
kernel, part A: 0 findings
kc: PASS x185 in 42 runs
m: PASS x185 in 42 runs
kcr:  in 0 runs
  Missing util(s): <recorded only>
kg   zfs_load-key,zfs_unload-key,zfs_change-key       kc: PASS x26               M: PASS x26
r01  rsend/recv_dedup_encrypted_zvol                  kc: PASS x3                M: PASS x3
...
dg   dedup                                            kc: PASS x19               M: PASS x19
KC pass 1: hits +<h1>, ...
KC pass 2: hits +<h2>, ...
flags: STOP [] HELD [] PANIC []
```

with no `on both`, `BRANCH ONLY` or `M only` line and no rerun. A test that
failed or was KILLED on KC only runs once more, alone, on KC (`kcr-<i>`, at
most 6): its result line follows `rerun on KC:`.

## STEP 6: part C on IV: list C, then phase xv1

```sh
. /var/log/zts-enc-1026/env.sh; part C
if ! blocked && switch_to IV; then
	run_list iv IV $L/listC.txt
	if ! blocked; then
		since=$(date '+%F %T')
		say "== xv1 on IV"
		cd $W/IV
		mkdir -p $XV/xvb $XV/xvm $XV/mnt-xvb $XV/mnt-xvm &&
		    dd if=/dev/urandom of=$XV/src bs=128k count=16 status=none &&
		    dd if=/dev/urandom of=$XV/key bs=32 count=1 status=none && chmod 600 $XV/key &&
		    truncate -s 1G $XV/xvb/v0 $XV/xvm/v0 || stop "xv1: files"
		echo 1 | sudo -n tee /sys/module/zfs/parameters/zfs_dedup_log_txg_max > /dev/null
		if ! blocked && sudo -n ./zpool create -o cachefile=none -m $XV/mnt-xvb xvb $XV/xvb/v0 &&
		    sudo -n ./zfs create -o encryption=on -o keyformat=raw -o keylocation=file://$XV/key \
		    -o dedup=sha256 -o compression=off -o recordsize=128k -o xattr=sa xvb/enc; then
			say "  xvb: feature@blake3 $(sudo -n ./zpool get -H -o value feature@blake3 xvb)," \
			    "zfs_crypt_dedup_blake3 $(cat $PIV)"
			xv_copy xvb xvb/enc/file1
			xv_copy xvb xvb/enc/file2
			xv_read xvb xvb/enc/file1 xvb/enc/file2
			sudo -n ./zfs snapshot xvb/enc@s1 && sudo -n ./zfs send -w xvb/enc@s1 > $XV/iv-raw.zsend ||
			    touch $PD/XVBAD
			say "  iv-raw.zsend: $(stat -c %s $XV/iv-raw.zsend) bytes"
			xv_down xvb xv1 16 0 1 && xv_check xv1
		else
			stop "xv1: pool or dataset create"
		fi
		kern_check xv1 "$since"
	fi
fi
step_end 6
```

Expected: `unload M rc=0`, `load IV rc=0`, `IV guard ok: srcversion
A8DA0D31A506E045083D17C, zfs_flags 4294932990, coresize <n>, kstat no, IV
tunable yes`, then the 36 runs labelled `iv-...`, each `PASS x<n>` with its
row of STEP 1 (`iv-de -t dedup/dedup_encrypted PASS x3`, `iv-dg -T dedup PASS
x20`), `list listC.txt done on IV`, then:

```
== xv1 on IV
  xvb: feature@blake3 enabled, zfs_crypt_dedup_blake3 1
  xvb/enc/file1: written
  xvb/enc/file2: written
  xvb/enc/file1: read back as src
  xvb/enc/file2: read back as src
  iv-raw.zsend: <about 2.2e6> bytes
  xv1: DDT duplicate 16 (expected 16), unique 0 (expected 0), log 0 (expected 0); keys recording DSL_CRYPTO_DEDUP_PRF = 1: 1 (expected 1); zdb -bcc rc=0, 1 'No leaks'
xv1: as expected
kernel xv1: 0 findings, 0 ZTS marks
STEP 6 done <time>
```

If `de` does not pass, its lines name the step (`copies`, `clones`,
`reloaded keys`, `raw-received copies`) and the DDT counts it saw. 25 to 40
minutes.

## STEP 7: part C on M: phase xv2, `dedup_encrypted` with IV's files, the rest of list C

```sh
. /var/log/zts-enc-1026/env.sh; part C
if ! blocked && switch_to M; then
	set_basedir M $PD/results | tee -a $PD/runs.txt
	since=$(date '+%F %T')
	say "== xv2 on M"
	if xv_up xvb; then
		xv_read xvb xvb/enc/file1 xvb/enc/file2
		xv_scrub xvb
		xv_copy xvb xvb/enc/file3
		xv_read xvb xvb/enc/file3
		xv_down xvb xv2b 16 16 1
	fi
	echo 1 | sudo -n tee /sys/module/zfs/parameters/zfs_dedup_log_txg_max > /dev/null
	if ! blocked && sudo -n ./zpool create -o cachefile=none -m $XV/mnt-xvm xvm $XV/xvm/v0 &&
	    sudo -n ./zfs create -o encryption=on -o keyformat=raw -o keylocation=file://$XV/key \
	    -o dedup=sha256 -o compression=off -o recordsize=128k -o xattr=sa xvm/enc; then
		xv_copy xvm xvm/enc/file1
		xv_copy xvm xvm/enc/file2
		xv_read xvm xvm/enc/file1 xvm/enc/file2
		xv_recv $XV/iv-raw.zsend xvm/fromiv
		xv_read xvm xvm/fromiv/file1 xvm/fromiv/file2
		sudo -n ./zfs snapshot xvm/enc@s1 && sudo -n ./zfs send -w xvm/enc@s1 > $XV/m-raw.zsend ||
		    touch $PD/XVBAD
		xv_down xvm xv2m 16 0 0
	elif ! blocked; then
		stop "xv2: xvm create"
	fi
	! blocked && xv_check xv2
	kern_check xv2 "$since"
	if ! blocked; then
		cd $MT
		cp -p tests/zfs-tests/include/tunables.cfg $L/orig-M/tunables.cfg &&
		    install -m 755 $W/IV/tests/zfs-tests/tests/functional/dedup/dedup_encrypted.ksh \
		    tests/zfs-tests/tests/functional/dedup/ &&
		    cp $W/IV/tests/zfs-tests/include/tunables.cfg tests/zfs-tests/include/tunables.cfg
		same "M: IV's test files in" "$(git hash-object tests/zfs-tests/tests/functional/dedup/dedup_encrypted.ksh) $(stat -c %a tests/zfs-tests/tests/functional/dedup/dedup_encrypted.ksh) $(git hash-object tests/zfs-tests/include/tunables.cfg)" \
		    "39bfb2005a247fbc83863b90658f2bdee4228549 755 b45e5ddcc94c23d8454adf7c345e442fef1d7fe7" | tee -a $PD/runs.txt
		if grep -q '^MISMATCH: M: IV' $PD/runs.txt; then
			stop "M: IV's test files not as expected"
		else
			zts m-de M -t dedup/dedup_encrypted 3
		fi
		cd $MT
		git checkout HEAD -- tests/zfs-tests/include/tunables.cfg
		rm -f tests/zfs-tests/tests/functional/dedup/dedup_encrypted.ksh
		same "M: its own test files back" "$(git hash-object tests/zfs-tests/include/tunables.cfg) $(ls tests/zfs-tests/tests/functional/dedup/dedup_encrypted.ksh 2>/dev/null | wc -l) $(git status --short --untracked-files=no | xargs)" \
		    "83eb6e8bd2ab4e8e069cfccc15ca9bf839039d11 0 M tests/test-runner/bin/test-runner.py.in" | tee -a $PD/runs.txt
		grep -q '^MISMATCH: M: its own' $PD/runs.txt && stop "M's test files not restored"
		run_list m M $L/listC.txt skip-done
	fi
fi
step_end 7
```

Expected: `unload IV rc=0`, `load M rc=0`, the guard line (`kstat no, IV
tunable no`), `ok: M results directory /var/log/zts-enc-1026/C/results`, then

```
== xv2 on M
  xvb imported: xvb - yes; xvb/enc available yes
  xvb/enc/file1: read back as src
  xvb/enc/file2: read back as src
  scrub xvb: scrub repaired 0B in <t> with 0 errors on <date>; No known data errors
  xvb/enc/file3: written
  xvb/enc/file3: read back as src
  xv2b: DDT duplicate 16 (expected 16), unique 16 (expected 16), log 0 (expected 0); keys recording DSL_CRYPTO_DEDUP_PRF = 1: 1 (expected 1); zdb -bcc rc=0, 1 'No leaks'
  xvm/enc/file1: written
  xvm/enc/file2: written
  xvm/enc/file1: read back as src
  xvm/enc/file2: read back as src
  xvm/fromiv: received raw from iv-raw.zsend, key loaded, mounted
  xvm/fromiv/file1: read back as src
  xvm/fromiv/file2: read back as src
  xv2m: DDT duplicate 16 (expected 16), unique 0 (expected 0), log 0 (expected 0); keys recording DSL_CRYPTO_DEDUP_PRF = 1: 0 (expected 0); zdb -bcc rc=0, 1 'No leaks'
xv2: as expected
kernel xv2: 0 findings, 0 ZTS marks
ok: M: IV's test files in
m-de M -t dedup/dedup_encrypted FAIL x1, PASS x2 (3 of 3 lines) rc=<n> wall=<s>s
  <its Test: line with [FAIL]>
    first failing line: <...>
    first ERROR line: ERROR: set_tunable32 CRYPT_DEDUP_BLAKE3 1 exited 1
kernel m-de: 0 findings, 3 ZTS marks
  zpool.cache: <...>
leftovers after m-de: none
ok: M: its own test files back
m-de: already run
m-dg: taken from part A (PASS x19 lines=19/19 rc=0)
m-kg: taken from part A (PASS x26 lines=26/26 rc=0)
m-p1 M -t cli_root/zpool_create/zpool_create_crypt_combos PASS x3 (3 of 3 lines) rc=0 wall=<s>s
...
m-f2 M -t cli_root/zfs_create/zfs_create_encrypted PASS x3 (3 of 3 lines) rc=0 wall=<s>s
m-r01: taken from part A (...)
...
list listC.txt done on M
STEP 7 done <time>
```

The `xvb imported:` line lists each dataset with its key status and mount
state (`xvb - yes` is the unencrypted root). `m-de`'s `rc` is the runner's
(non-zero after a failure, as for any FAIL). What each outcome of `m-de`
means is under "What the results mean". 10 to 20 minutes (more if part A did
not finish its runs on M).

## STEP 8: part C on IV again: phase xv3, and IV-only failures once more

```sh
. /var/log/zts-enc-1026/env.sh; part C
if ! blocked && switch_to IV; then
	since=$(date '+%F %T')
	say "== xv3 on IV"
	if xv_up xvb; then
		xv_read xvb xvb/enc/file1 xvb/enc/file2 xvb/enc/file3
		xv_copy xvb xvb/enc/file4
		xv_read xvb xvb/enc/file4
		xv_recv $XV/m-raw.zsend xvb/fromm
		xv_read xvb xvb/fromm/file1 xvb/fromm/file2
		xv_scrub xvb
		xv_down xvb xv3b 16 16 1
	fi
	if ! blocked && xv_up xvm; then
		xv_read xvm xvm/enc/file1 xvm/enc/file2 xvm/fromiv/file1 xvm/fromiv/file2
		xv_copy xvm xvm/enc/file3
		xv_read xvm xvm/enc/file3
		xv_scrub xvm
		xv_down xvm xv3m 16 0 0
	fi
	! blocked && xv_check xv3
	kern_check xv3 "$since"
	! blocked && rerun_branch_only iv IV $L/listC.txt
fi
step_end 8
```

Expected: `unload M rc=0`, `load IV rc=0`, the guard line, then

```
== xv3 on IV
  xvb imported: xvb - yes; xvb/enc available yes
  xvb/enc/file1: read back as src
  xvb/enc/file2: read back as src
  xvb/enc/file3: read back as src
  xvb/enc/file4: written
  xvb/enc/file4: read back as src
  xvb/fromm: received raw from m-raw.zsend, key loaded, mounted
  xvb/fromm/file1: read back as src
  xvb/fromm/file2: read back as src
  scrub xvb: ... with 0 errors ...; No known data errors
  xv3b: DDT duplicate 16 (expected 16), unique 16 (expected 16), log 0 (expected 0); keys recording DSL_CRYPTO_DEDUP_PRF = 1: 1 (expected 1); zdb -bcc rc=0, 1 'No leaks'
  xvm imported: xvm - yes; xvm/enc available yes; xvm/fromiv available yes
  <the five reads of xvm, the write and read of xvm/enc/file3, the scrub>
  xv3m: DDT duplicate 16 (expected 16), unique 0 (expected 0), log 0 (expected 0); keys recording DSL_CRYPTO_DEDUP_PRF = 1: 0 (expected 0); zdb -bcc rc=0, 1 'No leaks'
xv3: as expected
kernel xv3: 0 findings, 0 ZTS marks
reruns on IV: 0 branch-only failures, 0 rerun
STEP 8 done <time>
```

A test that failed on IV only (against M's result in this part or part A)
runs once more, alone, before the `reruns` line (`ivr-<i>`).

## STEP 9: part C on M again: phase xv4, and IV against M

```sh
. /var/log/zts-enc-1026/env.sh; part C
if ! blocked && switch_to M; then
	since=$(date '+%F %T')
	say "== xv4 on M"
	if xv_up xvb; then
		xv_read xvb xvb/enc/file1 xvb/enc/file2 xvb/enc/file3 xvb/enc/file4 xvb/fromm/file1 xvb/fromm/file2
		xv_scrub xvb
		xv_down xvb xv4b 16 16 1
	fi
	if ! blocked && xv_up xvm; then
		xv_read xvm xvm/enc/file1 xvm/enc/file2 xvm/enc/file3 xvm/fromiv/file1 xvm/fromiv/file2
		xv_scrub xvm
		xv_down xvm xv4m 16 0 0
	fi
	! blocked && xv_check xv4
	kern_check xv4 "$since"
fi
{
sudo -n journalctl -k --since "$(cat $L/start.txt)" --no-pager -q -o short-iso 2>&1 | kfilter > $PD/kernf-part.txt
echo "kernel since the start: $(grep -ciE "$KRE" $PD/kernf-part.txt) findings"
for p in iv m ivr; do
	echo "$p: $(cat $PD/$p-*.log 2>/dev/null | grep -E "$TRE" | tally) in $(grep -c "^$p-" $PD/zts-results.txt) runs here"
done
sort -u $PD/missing-utils.txt 2>/dev/null
compare_zts iv $L/listC.txt
grep '^ivr-' $PD/zts-results.txt | sed 's/^/rerun on IV: /'
grep -hE '^  xv[1-4][bm]?: |^xv[1-4]: ' $PD/runs.txt
echo "flags: STOP [$(cat $PD/STOP 2>/dev/null)] HELD [$(ls $L/HELD 2>/dev/null)] PANIC [$(ls $L/PANIC 2>/dev/null)]"
} 2>&1 | tee $PD/compare.txt
step_end 9
```

Expected: `unload IV rc=0`, `load M rc=0`, the guard line, the reads of the
six copies of `xvb` and the five of `xvm` all `read back as src`, both scrubs
clean, `xv4b` with 16 / 16 / 1 and `xv4m` with 16 / 0 / 0, `xv4: as
expected`, `kernel xv4: 0 findings`, then

```
kernel since the start: 0 findings
iv: PASS x144 in 36 runs here
m: FAIL x1, PASS x<2 + the lines of the runs not taken from part A> in <5 or more> runs here
ivr:  in 0 runs here
  Missing util(s): <recorded only>
de   dedup/dedup_encrypted                            iv: PASS x3                M: FAIL x1, PASS x2
       M only: dedup/dedup_encrypted FAIL
dg   dedup                                            iv: PASS x20               M: PASS x19
kg   zfs_load-key,zfs_unload-key,zfs_change-key       iv: PASS x26               M: PASS x26
...
<the xv lines of the four phases, each as expected>
flags: STOP [] HELD [] PANIC []
```

with no other `on both`, `BRANCH ONLY` or `M only` line.

## STEP 10: part B: the release trees, and the fio pool written once

Only with fio present and at least 6G free (the pool's file takes about
2.5G):

```sh
. /var/log/zts-enc-1026/env.sh; part B
say "fio: ${FIO:-absent} ($($FIO --version 2>/dev/null)); /var/tmp free: $(df -BG --output=avail /var/tmp | tail -1 | tr -d ' ');" \
    "mpstat $(command -v mpstat || echo absent); $(free -m | sed -n 2p)"
if [ -z "$FIO" ] || [ "$(df -BG --output=avail /var/tmp | tail -1 | tr -dc 0-9)" -lt 6 ]; then
	stop "part B not run: fio ${FIO:-absent}, $(df -BG --output=avail /var/tmp | tail -1 | tr -d ' ') free"
elif ! blocked && switch_to M-rel; then
	cd $W/M-rel
	E=$W/B/mnt/enc
	mkdir -p $W/B/vdev $W/B/mnt && truncate -s 6G $W/B/vdev/v0 &&
	    dd if=/dev/urandom of=$W/B/key bs=32 count=1 status=none && chmod 600 $W/B/key &&
	    sudo -n ./zpool create -o cachefile=none -m $W/B/mnt kcperf $W/B/vdev/v0 &&
	    sudo -n ./zfs create -o encryption=on -o keyformat=raw -o keylocation=file://$W/B/key \
	    -o recordsize=4k -o primarycache=metadata -o compression=off -o atime=off kcperf/enc &&
	    sudo -n chown "$(id -un):" $E && mkdir $E/old $E/dd || stop "prep: pool"
	for x in old dd; do
		blocked && break
		[ $x = dd ] && { sudo -n ./zfs set dedup=on kcperf/enc || stop "prep: dedup=on"; }
		t0=$(date +%s)
		nice -n 10 ionice -c3 $FIO --name=$x --directory=$E/$x --rw=write --bs=4k --size=64m \
		    --numjobs=16 --end_fsync=1 $([ $x = dd ] && echo --refill_buffers) --group_reporting \
		    --output-format=json --output=$PD/write-$x.json < /dev/null > $PD/write-$x.err 2>&1 ||
		    stop "prep: fio write $x"
		say "write $x: $(python3 -I -c 'import json, sys
j = json.load(open(sys.argv[1]))["jobs"][0]
print("%d MiB, %d KiB/s, error %d" % (j["write"]["io_kbytes"] / 1024, j["write"]["bw"], j["error"]))' \
		    $PD/write-$x.json 2>&1), wall $(( $(date +%s) - t0 ))s"
	done
	if ! blocked; then
		sudo -n ./zfs unmount kcperf/enc && sudo -n ./zfs unload-key kcperf/enc &&
		    sudo -n ./zfs load-key kcperf/enc > /dev/null && sudo -n ./zfs mount kcperf/enc &&
		    say "key unloaded and loaded again: old/ is now under an older salt" || stop "prep: key reload"
		t0=$(date +%s)
		! blocked && perf_hash prep &&
		    say "reference hashes: old $(cut -c1-16 $PD/hash-old-prep.txt)..., dd $(cut -c1-16 $PD/hash-dd-prep.txt)..., read in $(( $(date +%s) - t0 ))s"
		say "space: $(sudo -n ./zfs get -Hp -o value used kcperf/enc) bytes used, dedupratio $(sudo -n ./zpool get -H -o value dedupratio kcperf), vdev file $(du -m $W/B/vdev/v0 | cut -f1) MiB"
		perf_down prep
	fi
fi
step_end 10
```

Expected: the fio line (`fio-3.<n>`), the free space (at least 6G), the
memory line; `unload M rc=0`, `load M-rel rc=0`, `M-rel guard ok: srcversion
8852710DB871A7587B9E722, zfs_flags 0, coresize <n>, kstat no, IV tunable no`;
`write old: 1024 MiB, <n> KiB/s, error 0, wall <s>s`, `write dd: 1024 MiB,
...`; `key unloaded and loaded again: ...`; `reference hashes: ...`; `space:
<about 2.2e9> bytes used, dedupratio 1.00x, vdev file <about 2400> MiB`; `ok:
prep down`; `STEP 10 done`. About 5 to 10 minutes.

## STEP 11: part B: five rounds, M-rel then KC-rel

```sh
. /var/log/zts-enc-1026/env.sh; part B
if grep -q '^STEP 10 done' $L/steps.txt && ! blocked; then
	for n in 1 2 3 4 5; do
		for t in M-rel KC-rel; do
			since=$(date '+%F %T')
			say "== round $n $t $(date '+%T') load $(cut -d' ' -f1-3 /proc/loadavg)"
			switch_to $t && perf_up $n-$t || break 2
			for x in old dd; do
				$(fio_cmd $x 5 $PD/warm-$n-$t-$x.json) < /dev/null > $PD/warm-$n-$t-$x.err 2>&1 ||
				    stop "$n $t: warm-up $x"
			done
			! blocked && perf_phase $n $t old && perf_phase $n $t dd && perf_phase $n $t mix || break 2
			[ $n = 5 ] && perf_hash 5-$t
			perf_down $n-$t && kern_check perf-$n-$t "$since" || break 2
		done
	done
fi
step_end 11
python3 -I $L/table.py $PD/results.tsv $RT | tee $PD/table.txt
```

Expected, for each of the 10 loads:

```
== round <n> <tree> <time> load <loadavg>
unload <previous tree> rc=0
load <tree> rc=0
<tree> guard ok: srcversion <its srcversion>, zfs_flags 0, coresize <n>, kstat <yes on KC-rel, no on M-rel>, IV tunable no
ok: <n>-<tree> up
<n> <tree> old IOPS <n> lat <x>us sys/usr/steal <x>/<x>/<x>% pgpgin <n>KiB kstat +[<hits misses admissions evictions> or n/a n/a n/a n/a]
<n> <tree> dd IOPS ...
<n> <tree> mix-old IOPS ...
<n> <tree> mix-dd IOPS ...
ok: <n>-<tree> down
kernel perf-<n>-<tree>: 0 findings, 0 ZTS marks
```

with, in round 5, `ok: 5-M-rel old read back as M-rel read it at the
preparation` and the same for `dd` and for `5-KC-rel`; then `STEP 11 done`
and the table (median [min-max] over the 5 rounds per phase and tree, the
KC/M change of each phase's median, the share of its alone rate each job
keeps beside the other, and the per-round values). Checks: the guard, `ok:`
and kernel lines, and the four hashes. Recorded only: the numbers. On KC-rel
the kstat should show, in `old`, hits near IOPS x 30 and a handful of misses
(the salt is admitted once per load, in the warm-up); in `dd`, misses near
IOPS x 30 and hits near 0. `pgpgin` near 0 means the reads came from the page
cache, so the phase was bound by the CPU. About 2.5 minutes per load, 25 to
30 minutes in all.

## STEP 12: part D: ztest, M then IV, three rounds

```sh
. /var/log/zts-enc-1026/env.sh; part D
if ! blocked && switch_to M; then
	sudo -n chmod 0666 /dev/zfs
	say "/dev/zfs: $(stat -c '%a %U' /dev/zfs)"
	since=$(date '+%F %T')
	for n in 1 2 3; do
		s=
		[ $n = 1 ] && s=$ZSEED
		ztest_run zt$n-M M $s && ztest_run zt$n-IV IV $s || break
	done
	kern_check ztest "$since"
fi
step_end 12
```

Expected: `unload KC-rel rc=0` (the last tree part B loaded; `M already
loaded` if part B did not run), `load M rc=0`, the guard line, `/dev/zfs: 666
root`, then six blocks like

```
zt1-M M seed bd69993f1c9411d6...: rc=0 wall=<s>s; 1 vdevs, 7 datasets, 23 threads, 16 draid disks, parity 1, 300 seconds...; <n> passes (<n> SIGKILLed); seed bd69993f1c9411d6...; final zdb: no failure reported
```

(`zt1-IV` with the same header, which the seed decides; rounds 2 and 3
without `seed` after the tree, each with the seed and layout ztest drew),
then `kernel ztest: 0 findings` and `STEP 12 done`. A run whose final zdb
failed prints ztest's `exit code` line, then `zdb again rc=<n>: ERROR lines
<n>, livelist lines <n>` and those lines (zdb's own output, which ztest does
not show). `rc` 0 is a pass; any other `rc`, an `ERROR` or a livelist line is
a result to report, not a stop. Each run takes 5 to 8 minutes (300 s, then
ztest's zdb check), about 40 in all.

`/dev/zfs` is made 0666 as an installed system's `90-zfs.rules` makes it:
the trees are not installed, so the node is root's only, and ztest runs as
this user. The node goes away with the module in STEP 13.

## STEP 13: final checks, cleanup and report

Run these blocks in all cases. First the whole task's kernel check, the
runners' files restored in the three debug trees, and the unload (the module
stays loaded on `PANIC` or `HELD`):

```sh
. /var/log/zts-enc-1026/env.sh; part D
{
sudo -n journalctl -k --since "$(cat $L/start.txt)" --no-pager -q -o short-iso 2>&1 | kfilter > $L/kernf-all.txt
echo "kernel, whole task: $(grep -ciE "$KRE" $L/kernf-all.txt) findings, $(grep -c '^DEPOT SPLAT' $L/kernf-all.txt) depot splats"
grep -iE "$KRE" $L/kernf-all.txt | head -n 40
cat $L/steps.txt
for t in KC IV M; do
	r=$(tdir $t)/tests/test-runner/bin/test-runner.py
	if [ -e $L/orig-$t/test-runner.py ]; then
		cp -p $L/orig-$t/test-runner.py.in $r.in && cp -p $L/orig-$t/test-runner.py $r
		echo "$t runner restored rc=$?"
	fi
done
cd $MT && git checkout HEAD -- tests/zfs-tests/include/tunables.cfg &&
    rm -f tests/zfs-tests/tests/functional/dedup/dedup_encrypted.ksh
same "M: no tracked change, no copied test" "$(git -C $MT status --short --untracked-files=no | wc -l) $(ls $MT/tests/zfs-tests/tests/functional/dedup/dedup_encrypted.ksh 2>/dev/null | wc -l)" "0 0"
same "M: runner as found" "$(grep -n '^BASEDIR' $MT/tests/test-runner/bin/test-runner.py.in $MT/tests/test-runner/bin/test-runner.py | xargs)" \
    "$MT/tests/test-runner/bin/test-runner.py.in:43:BASEDIR = '/var/tmp/test_results' $MT/tests/test-runner/bin/test-runner.py:43:BASEDIR = '/var/tmp/test_results'"
for t in KC IV; do
	same "$t: no tracked change" "$(git -C $W/$t status --short --untracked-files=no | wc -l)" 0
done
t=$(cat $L/loaded-tree 2>/dev/null)
if [ -e $L/PANIC ] || [ -e $L/HELD ]; then
	echo "module left loaded ($t): $(ls $L/PANIC $L/HELD 2>/dev/null | xargs)"
elif [ -n "$t" ] && [ "$(cat /sys/module/zfs/srcversion 2>/dev/null)" = "$(cat $L/srcv-$t)" ]; then
	p=$(cd $(tdir $t) && sudo -n ./zpool list -H -o name 2>&1)
	if [ -n "$p" ]; then
		echo "pools still imported: $p; module left loaded"
	else
		(cd $(tdir $t) && nice -n 10 ionice -c3 sudo -n ./scripts/zfs.sh -u) > $L/unload-final.log 2>&1
		echo "unload $t rc=$?"
		rm -f $L/loaded-tree
	fi
else
	echo "no module of this task is loaded"
fi
echo "lsmod: [$(lsmod | grep -E '^(zfs|spl) ' | awk '{ print $1 }' | xargs)]"
pgrep -ax 'zed|lt-zed'
others
echo "loops: [$(loops | xargs)]; dm: $(sudo -n dmsetup ls 2>&1)"
ls -l /etc/zfs/vdev_id.conf /tmp/ZTS-snakeoil.log $(cat $L/zedpid.txt) 2>&1
echo "zpool.cache: $(stat -c '%s bytes' /etc/zfs/zpool.cache 2>/dev/null || echo absent)"
ls -A / | diff $L/root-before.txt - && echo "/ as at the start"
} 2>&1 | tee $L/end.txt
```

Expected: `kernel, whole task: 0 findings, 0 depot splats`, no kernel line,
the step lines (`STEP 3 done` to `STEP 12 done`), `KC runner restored rc=0`, `IV runner restored rc=0`, `M runner
restored rc=0`, four `ok:` lines, `unload M rc=0`, `lsmod: []`, nothing for
`zed` and processes, `loops: []; dm: No devices found`, `No such file or
directory` for the three files, `zpool.cache` absent or 0 bytes, `/ as at the
start`.

Then remove this task's scratch, unless the module stays loaded: the pools'
files, keys and streams, and the two `-d` directories. The five trees and
`repo` stay (for reruns; the maintainer decides about them), and so does
`$L`:

```sh
. /var/log/zts-enc-1026/env.sh; part D
{
if [ ! -e $L/PANIC ] && [ ! -e $L/HELD ] && ! lsmod | grep -qE '^(zfs|spl) '; then
	rm -rf /var/tmp/zts-enc-1026/A /var/tmp/zts-enc-1026/B /var/tmp/zts-enc-1026/C /var/tmp/zts-enc-1026/D
	rmdir /var/tmp/zts-enc-1026/files-A /var/tmp/zts-enc-1026/files-C
	echo "scratch removed rc=$?"
else
	echo "scratch kept: the module is loaded or a flag is set"
fi
echo "W holds: $(LC_ALL=C ls -A $W | xargs)"
echo "/var/tmp free: $(df -BG --output=avail /var/tmp | tail -1 | tr -d ' ')"
echo "logs: $(du -sh $L | cut -f1) in $L"
} 2>&1 | tee -a $L/end.txt
```

Expected: `scratch removed rc=0` (`rmdir` fails if a `-d` directory is not
empty: then report it), `W holds: IV KC KC-rel M-rel repo`, the free space
(about what STEP 0 found), the log size.

Write the report:

```sh
. /var/log/zts-enc-1026/env.sh; part D
{
echo "VM 107, kernel tests of zio-crypt-salt-key-cache d511c2638b and dedup-iv-blake3 6fcd458a73"
echo "against master a128b8a075 (n/VM-TESTS-107-enc.md)"
for s in step0:"STEP 0" files:"STEP 1 files" checks:"STEP 1 checks" baseline:"STEP 2" steps:"steps" \
    A/runs:"part A runs" A/compare:"part A: KC against M" C/runs:"part C runs" C/compare:"part C: IV against M" \
    B/runs:"part B runs" B/table:"part B table" D/runs:"part D runs" end:"STEP 13"; do
	echo; echo "== ${s#*:}"
	cat $L/${s%%:*}.txt 2>&1
done
echo; echo "== part B rows (round tree phase iops lat_us sys/usr/steal pgpgin kstat-deltas)"
cat $L/B/results.tsv 2>&1
} > $L/REPORT.txt
wc -l $L/REPORT.txt
```

Then add, by hand, at the end of `REPORT.txt`: a `== summary` section naming,
for each part, the case of "What the results mean" that applies, with the
figures it rests on (part A: the kstat deltas of both passes and the ZTS
totals; part C: `de` on IV and on M, the totals, the xv phases; part B: the
medians of each phase and tree, the KC/M changes and the shares kept beside
the other job, the highest `%steal`; part D: the six `rc` values and any
`ERROR` or livelist line), and a `== deviations` section with anything that
differed from these steps and why (`none` if nothing did). Then `cat
/var/log/zts-enc-1026/REPORT.txt` and reply with the whole of it. Nothing runs
after that reply.

## What the results mean

Part A (KC):

- **As expected**: 185 of 185 PASS on KC and on M, the salt check's files
  read back in both passes on both trees, on KC each pass adds at least 1000
  hits with at least 16 admissions and some evictions, and no kernel line.
  The cache works in the kernel under rotating salts, with more salts than
  slots (so entries are evicted while reads use them), and the encryption
  tests that reach it pass on the ICP backend under real concurrency.
- **The kstat's numbers fall outside the bounds** (fewer hits, or misses
  close to the number of reads): the cache is not taken where it should be.
  Report the deltas; the data still read back, so it is a performance
  finding, not a correctness one.
- **A file reads back differently, or a test fails on KC only** (and its
  rerun on KC fails too): the branch's until shown otherwise. A wrong key for
  an older salt shows as a checksum or MAC error, `EIO`, or a received stream
  that differs; a `z1`/`z2` failure points at the userspace build.
- **A test fails on both trees**: master's or this VM's (fio, openssl, the
  https fetcher, zvol links), not the change's. **On M only**: reported; it
  says nothing against the branch. **A rerun that passes**: a flaky test,
  reported as such.
- **Any VERIFY, ASSERT, panic, oops or hung task**: the most important result
  of the whole run. A `zio_crypt_salt_*` function, `zsc_lock` or
  `zsc_ghost_lock` in it is KC's; the ASSERT in
  `zio_crypt_salt_cache_destroy()` firing means an entry's reference leaked.

Part C (IV):

- **As expected**: `de` passes on IV; the 36 runs match M (`dg` with one test
  more on IV); every copy in both pools reads back as `src` on both trees,
  scrubs are clean, `zdb -bcc` finds no leak, and the DDT and record counts
  match the table. That shows, in the kernel: a new key records keyed BLAKE3
  and its copies deduplicate; master reads, scrubs and writes such a pool, and
  its own writes there (HMAC) do not collide with the BLAKE3 blocks; IV comes
  back to the same key and deduplicates against its own earlier blocks again
  (`file4` adds no entry), not against master's (`file3` stays unique); a
  key without the record (created by master, or received from master's raw
  stream) stays HMAC on IV, byte for byte as master derives it (`xvm`'s
  `file3` written by IV deduplicates against master's `file1`-`2`); master
  drops the record when it receives IV's raw stream; IV records nothing when
  it receives master's.
- **`m-de` FAILs with `set_tunable32 CRYPT_DEDUP_BLAKE3 1` as its first
  ERROR** (expected): the test needs the branch's tunable, so it cannot pass
  on master. **`m-de` PASSes**: impossible with master's module, so the run
  was not on M: report, the comparison of part C is void. **`m-de` FAILs
  earlier** (pool create, `feature@blake3` not enabled): the environment;
  compare with `iv-de`.
- **`iv-de` FAILs**: its first ERROR line names the step: `copies` (keyed
  BLAKE3 or HMAC blocks do not deduplicate at all), `clones`, `reloaded keys`
  (the derived BLAKE3 key changed across a key load), `raw-received copies`
  (the record did not travel), or `blake3_keys` (the record was not written,
  or written for the HMAC key too). Each is IV's.
- **A cross-version count differs**: `xv1` unique not 0, IV's copies did not
  deduplicate; `xv2b` duplicate 32, master derived the same salt as BLAKE3
  (impossible) or wrote over; `xv3b` duplicate 32 and unique 0, IV fell back
  to HMAC on its own BLAKE3 key; `xv3b` unique 32, IV derived a third value;
  `xv3m` unique 16, IV used BLAKE3 on a key without the record. Any copy
  that does not read back, a scrub error or a `zdb -bcc` failure is a
  correctness failure, whichever tree wrote it.
- **A test fails on IV only**, as for KC: the branch's until shown
  otherwise, especially in `kg`, `p1`, `p2`, `f1`, `f2`, `c03`-`c07` and the
  raw `r` runs, which go through the changed key creation, load and raw
  receive code.

Part B (indicative, shared hypervisor):

- **`old`**: KC-rel well above M-rel (master derives a key per 4K block; in
  userspace that was 57-59% of the time of a 4K decrypt), with the kstat's
  hits at about IOPS x 30.
- **`dd`**: KC-rel within about 10% of M-rel. Every block misses, takes
  `zsc_lock` as reader and `zsc_ghost_lock`, and is never admitted; much
  lower means the miss path contends at 16 jobs, which is what the ghost
  lock's own mutex was for (the first version ran misses 13 times slower).
- **Beside the other job**: on KC-rel, `old` keeps about as large a share
  of its alone rate as on M-rel, or larger, since its jobs no longer derive
  keys; a much smaller share on KC-rel means hits wait behind misses.
- **Medians within a few percent with overlapping ranges**: no measurable
  difference here. `%steal` above a few percent: the host was busy then; say
  so next to that phase. `pgpgin` far above 0: that phase read the disk.
- **A hash mismatch**: a correctness failure, whatever the speed.

Part D:

- **Every run rc 0, no `ERROR` or livelist line**: the error is not
  reproduced on IV, and master's runs reach the same zdb check; the notes'
  finding stays unexplained and rare. **A livelist error on IV only**:
  worth investigating with that run's seed and log. **On both trees, or on
  M only**: master's, not the branch's. **"no pools found"** in spite of the
  loaded module: a real finding, not the container artifact. Other failures
  of ztest are reported with their lines and compared between the trees.

## REPORT.txt expected

`<...>` is what the run fills in; everything else is what these instructions
expect.

```
VM 107, kernel tests of zio-crypt-salt-key-cache d511c2638b and dedup-iv-blake3 6fcd458a73
against master a128b8a075 (n/VM-TESTS-107-enc.md)

== STEP 0
<the STEP 0 output>

== STEP 1 files
<the five sha256 lines of STEP 1>

== STEP 1 checks
<34 ok lines and the recorded lines>
list A on KC: 42 runs, 185 result lines; flags: []
list A on M: 42 runs, 185 result lines; flags: []
list C on IV: 36 runs, 144 result lines; flags: []
list C on M: 36 runs, 143 result lines; flags: [de  MISSING UNLISTED]
ok: KC results directory /var/log/zts-enc-1026/A/results
ok: M results directory /var/log/zts-enc-1026/A/results
ok: IV results directory /var/log/zts-enc-1026/C/results

== STEP 2
baseline at <date>: loops [], dm [No devices found], journal (this boot) <n> suspicious lines, <n> depot splats; / holds <n> entries; zpool.cache <...>

== steps
STEP 3 done <time>
STEP 4 done <time>
STEP 5 done <time>
STEP 6 done <time>
STEP 7 done <time>
STEP 8 done <time>
STEP 9 done <time>
STEP 10 done <time>
STEP 11 done <time>
STEP 12 done <time>

== part A runs
<STEP 3 and 4 lines: the loads and guards, the salt checks, 2 x 42 runs each PASS, with kernel and leftovers lines>

== part A: KC against M
kernel, part A: 0 findings
kc: PASS x185 in 42 runs
m: PASS x185 in 42 runs
kcr:  in 0 runs
<42 label lines, no on both, BRANCH ONLY or M only line>
KC pass 1: hits +<h1 >= 1000>, misses +<20..400>, admissions +<>= 16>, evictions +<>= 1>
KC pass 2: hits +<h2 >= 1000>, ...
flags: STOP [] HELD [] PANIC []

== part C runs
<STEP 6 to 9 lines>

== part C: IV against M
kernel since the start: 0 findings
iv: PASS x144 in 36 runs here
m: FAIL x1, PASS x<n> in <n> runs here
ivr:  in 0 runs here
de   dedup/dedup_encrypted   iv: PASS x3   M: FAIL x1, PASS x2
       M only: dedup/dedup_encrypted FAIL
<35 label lines, no other on both, BRANCH ONLY or M only line>
<the xv lines: xv1 16/0/1, xv2b 16/16/1, xv2m 16/0/0, xv3b 16/16/1, xv3m 16/0/0, xv4b 16/16/1, xv4m 16/0/0; each phase as expected>
flags: STOP [] HELD [] PANIC []

== part B runs
<STEP 10 and 11 lines>

== part B table
<the table>

== part D runs
<the six ztest lines and their detail lines, kernel ztest: 0 findings>

== STEP 13
<as expected in STEP 13>

== part B rows (round tree phase iops lat_us sys/usr/steal pgpgin kstat-deltas)
<40 rows>

== summary
A: <case>; KC kstat pass 1 +<h>/<m>/<a>/<e>, pass 2 ...; ZTS 185/185 PASS on KC and M
C: <case>; iv-de PASS, m-de FAIL at set_tunable32 CRYPT_DEDUP_BLAKE3 1; 144/144 PASS on IV; xv1-xv4 as expected
B: <case>; old KC/M <+x>%, dd <+-x>%, old keeps <x>% (M-rel) / <x>% (KC-rel) beside dd; steal max <x>%
D: <case>; rc M <r1> <r2> <r3>, IV <r1> <r2> <r3>; livelist lines <n>

== deviations
none
```

## Estimated duration

About 3 h 50 min (3 h 15 to 4 h 45): STEPS 0-2 about 10 min, STEP 3 35-50,
STEP 4 35-50, STEP 5 2 (plus 1-5 per rerun), STEP 6 25-40, STEP 7 10-20,
STEP 8 5, STEP 9 5, STEP 10 5-10, STEP 11 25-30, STEP 12 about 40, STEP 13
about 5.
