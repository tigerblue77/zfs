# VM 105: kernel ZTS and an fio A/B run for zio-crypt-salt-key-cache

These instructions are for the session on VM 105 (Debian 13, kernel 6.12, 16
CPUs, about 7 GiB of RAM, booted with `slub_debug=FZPU`). They give the fork
branch `tigerblue77/zfs:zio-crypt-salt-key-cache` what only a kernel can
show. Its one commit, `3239e81a1d` "zio_crypt: cache keys derived for older
salts", sits on upstream master `a128b8a075` "Linux 7.3 compat: META" and
changes `include/sys/zio_crypt.h`, `module/zfs/zio_crypt.c` and the three
crypto backends (5 files, 335 insertions, 10 deletions). Until now it has run
in userspace only (libzpool: ztest, a harness, ASan); no kernel has loaded it.
The note written with the change for a disposable VM ("VM-NOTE" below) lists
the tests that reach the changed paths and the fio recipe; everything this
session needs from it is in this document.

What the change does: `zio_do_crypt_data()` had a ready key only for the salt
the key currently writes with; every block written under another salt (every
block written before the last key load) paid an HKDF and a new ICP key
schedule per read. Now each loaded key keeps the derived keys and ICP context
templates of up to 16 other salts, admitted on a salt's second decryption miss
among the last 32 missed salts (encryption only looks up), reference counted,
behind a new rwlock `zsc_lock` that is never held with `zk_salt_lock`, and
freed with the key. Nothing changes on disk or in send streams.

The session runs two things, in this order, one at a time, and writes every
result to `/var/log/zts-kc-1026/REPORT.txt`, whose expected contents end this
document:

1. **ZTS on debug builds**, tree B first, then tree M: the encryption tests
   that reach the changed paths, one `zfs-tests.sh` run at a time, each with
   its own kernel check and leftovers check. Any test that fails on B runs
   once more on M, alone.
2. **An fio A/B run on release builds** (VM-NOTE section 3, items 1 and 2):
   random reads of data written before the last key load, at recordsize 16k
   and 4k, alternating the M and B release modules over 5 rounds each,
   recording IOPS, `%sys` and, if `bpftrace` is installed, the number of
   `hkdf_sha512()` calls per read.

| Tree | Path | Commit | Build | Used for |
| --- | --- | --- | --- | --- |
| `B` | `/var/tmp/zts-kc-1026/B` | `3239e81a1d` (the branch) | `--enable-debug`, already built | ZTS |
| `M` | `/var/tmp/zts-kc-1026/M` | `a128b8a075` (its parent, master) | `--enable-debug`, already built | ZTS |
| `B-rel` | `/var/tmp/zts-kc-1026/B-rel` | `3239e81a1d` | release, built in STEP 6 | fio |
| `M-rel` | `/var/tmp/zts-kc-1026/M-rel` | `a128b8a075` | release, built in STEP 6 | fio |

`B` and `M` were built in place before this document (`./autogen.sh &&
./configure --enable-debug && make -j8` under nice and ionice; logs
`/var/log/zts-kc-1026/build-B.log` and `build-M.log`): rc=0 and no error on
both, srcversions `89263B1C179E680CDDE56B0` (B) and `8852710DB871A7587B9E722`
(M), and 83 `warning:` lines per tree, all from objtool (76 about `spl_panic()`
and 7 about `luaD_throw()` lacking `__noreturn`, the same on both trees and
known on this kernel), with no compiler warning of the `file:line:col:
warning:` form. Do not rebuild them.

## The tests, and why these

The commit changes no file under `tests/`, so both trees run the same suite;
STEP 1 checks that, and checks every count below against each tree's own
`common.run` and `linux.run`. From VM-NOTE's list, these are the tests that
write or read encrypted data, so that they reach `zio_do_crypt_data()` with
salts other than the current one:

| Label | Run | What it reaches | Test: lines |
| --- | --- | --- | --- |
| `kg` | `-T zfs_load-key,zfs_unload-key,zfs_change-key` | key loads and unloads (the cache is created in `zio_crypt_key_unwrap()` and freed in `zio_crypt_key_destroy()`), reads after a reload: the main path | 26 (20 tests, 3 setups, 3 cleanups) |
| `r01`-`r15` | `-t`, 15 of the 84 `rsend` tests | raw sends and receives (they keep the sender's salts) and sends of encrypted data; `r01` `recv_dedup_encrypted_zvol` reads back, whole, an encrypted zvol received from a deduplicated stream, and `r02` `send-wR_encrypted_zvol` writes with `dedup=on` (every block its own salt: the case the admission filter is for) | 3 each (setup, test, cleanup) |
| `c01`-`c10` | `-t`, 10 of the 33 `zfs_receive` tests | the 7 raw or encrypted receives, and the 3 corrective receives, which re-encrypt with each block's own salt (`dmu_recv.c:1579`), on an `aes-256-ccm` dataset | 3 each |
| `l2` | `-t l2arc/persist_l2arc_002_pos` | L2ARC on an encrypted dataset: re-encryption with the block's salt (`arc.c:9606`), L2ARC reads (`arc.c:8845`), and a reimport; needs fio | 3 |
| `s1`, `s2` | `-t zpool_scrub_encrypted_unloaded`, `zpool_scrub_thorough` | the only 2 of the 18 `zpool_scrub` tests with encrypted data; the thorough scrub decrypts | 3 each |
| `i1`-`i5` | `-t`, 5 of the 57 `zpool_import` tests | import, key load, then reads of what the previous import wrote; `errata3` and `errata4` import pool images written by older code (every block an old salt; errata 3 has the version 0 key format) | 3 each |
| `k1`, `k2` | `-t block_cloning_cross_enc_dataset`, `block_cloning_replay_encrypted` | cloned blocks read through another dataset after the source's key is unloaded; ZIL replay on an encrypted dataset after a reimport | 3 each |
| `z1`, `z2` | `-t zdb_encrypted`, `zdb_encrypted_raw` | `zdb -K`: the branch's code in userspace libzpool, decrypting what the kernel wrote; `cli_root/zdb` has no setup or cleanup | 1 each |
| `xg` | `-T send_xdr_encoding` | 7 of its 14 tests send encrypted datasets raw | 16 |
| `sg` | `-T zfs_send` | `zfs_send_encrypted`, `zfs_send_encrypted_unloaded` and `zfs_send_raw` among its 13 | 15 |
| `dg` | `-T dedup` | a **control**: no test of the group creates an encrypted dataset at these commits (`grep -l 'encryption=' tests/functional/dedup/*` finds none), so it reaches no changed code; encrypted dedup is `r01` and `r02` | 19 (common.run's 16, then linux.run's setup, `dedup_legacy_gang`, cleanup) |

That is 41 runs and 183 `Test:` lines per tree. Left out of VM-NOTE's list:
`zfs_create` and `zpool_create` (their `*_crypt_combos` tests create a dataset
of each cipher and read nothing written under an older salt; the rest is
unencrypted), `crypto` (userspace self-tests of the ICP, whose code is
unchanged), `userquota` (its setup adds system users and groups, which writes
to `/etc`), and the unencrypted bulk of `rsend`, `zfs_receive`, `l2arc`,
`zpool_scrub`, `zpool_import`, `zdb` and `block_cloning` (about 45 minutes of
CI time that cannot reach the change).

Sizes and timeouts: no selected section of the runfiles sets a timeout, so
every test has the default 600 s (as does every `-t` run); the runs use
`zfs-tests.sh`'s default disks, three sparse 4 GiB files under
`/var/tmp/zts-kc-1026/files` behind loop devices. The largest footprint is
`l2`, whose setup writes two 1 GiB files in full, plus a 400 MiB cache and
800 MB of fio data; no run needs more than about 3 GiB at once. In the
upstream CI (4-CPU debug VMs) these tests take about 8 minutes in all; at two
to four times that here (`slub_debug`, nice), plus about 15 s of
`zfs-tests.sh` overhead per run, a tree takes about 30 to 45 minutes.

Expected on both trees: every line PASS. Two environment outcomes would show
the same way on both trees and are not the change's: `zfs_load-key_https`
fails if libzfs was built without an https fetcher (STEP 1 prints the
`LIBFETCH_IS_*` lines), and `persist_l2arc_002_pos` is a SKIP without fio.
The uninstalled trees have no `compatibility.d`; none of the selected tests
sets `compatibility=`, and a failure that mentions it is the environment only
when M fails the same way.

## The fio part

VM-NOTE's recipe, adapted to this VM (20 GB free on `/var/tmp`, 7 GiB of
RAM):

- One pool, `kcperf`, on one sparse 8 GiB file `$W/perf/vdev/vdev0`, made with
  `cachefile=none` and mounted under `$W/perf/mnt`: it lives under the work
  directory and touches neither `/etc/zfs/zpool.cache` nor `/`.
- Two encryption roots (`encryption=on`, AES-256-GCM, a raw key in
  `$W/perf/key`), so that each has its own key and its own cache:
  `kcperf/r16k` (`recordsize=16k`, 4 files of 512 MiB, 2 GiB) and `kcperf/r4k`
  (`recordsize=4k`, 4 files of 256 MiB, 1 GiB), both `primarycache=metadata`
  (every read decrypts), `compression=off` (fio's data does not compress) and
  `atime=off` (reads write nothing). About 3.1 GiB on disk: under 40% of the
  vdev, and it stays in the page cache of the vdev file, so the reads are
  bound by the CPU, which is what the change saves. `pgpgin` per phase shows
  whether that held.
- Written once by fio on M-rel, then each key unloaded and loaded again, as
  VM-NOTE says: every block is now under an older salt, one salt per dataset,
  the change's main case (data written in an earlier key session). M-rel then
  reads everything back for a reference sha256; B-rel reads it back at the
  end, and the hashes must match.
- 5 rounds, each loading M-rel and then B-rel afresh (10 loads, each with an
  empty ARC): import, load both keys (a new current salt each time), mount,
  then per dataset a 10 s warm-up (it fills the ARC with the metadata, lets B
  admit the salt, and is where `bpftrace` counts `hkdf_sha512()`, so that its
  probe never weighs on a measured phase) and a measured 30 s
  `fio --rw=randread --numjobs=4 --iodepth=32 --ioengine=libaio`; then
  export. The same parameters must read the same at every load.
- Recorded per phase: IOPS, reads, mean latency, `%sys`, `%usr` and `%steal`
  over all CPUs from `/proc/stat` (the counters `mpstat` reads), mpstat's
  own average `%sys` when mpstat is installed, KiB paged in, and the warm-up's
  `hkdf_sha512()` calls when bpftrace is installed. Neither tool is installed
  if absent.

`slub_debug=FZPU` stays on, so the absolute numbers are pessimistic for both
trees; the A/B is fair (same pool, files, salts, parameters, cache state and
order). One bias to keep in mind: M allocates an ICP key schedule per block
read and B does not, and `slub_debug` makes every allocation dearer, so the
B/M ratio here probably overstates the gain on a production kernel.

## Rules for the whole task

- Log in as `tigerblue77` (passwordless sudo, not root) and run everything as
  that user, in bash, in this order; use `sudo -n` only where it is written.
- Post nothing, push nothing, install nothing (no package, no `make
  install`), never run `scripts/zfs-helpers.sh`, never use `zfs-tests.sh -x`,
  never reboot. Rebuild neither B nor M.
- Work only under `/var/tmp/zts-kc-1026` (`$W`) and log to
  `/var/log/zts-kc-1026` (`$L`). Never read, write or delete another task's
  directories (`/var/tmp/test_results`, other `/var/tmp/zts-*` or
  `/var/log/zts-*`): the ZTS results go to `$L/results`. The only `rm -rf` is
  the one in STEP 9, with its literal path.
- `/etc/zfs/zpool.cache` exists as a 0-byte file left by earlier tasks.
  Never delete, edit or rewrite it by hand. The pools ZTS creates use the
  default cache file, so the kernel rewrites it while they exist and
  truncates it to 0 bytes when the last one goes, as in every earlier run;
  each run records its size. The fio pool uses no cache file.
- Leave every file, process, tmux pane, loop device, pool and module you did
  not create alone. Never run a `zpool` or `zfs` command while none of this
  task's modules is loaded: libzfs would `modprobe` whatever zfs module the
  system has. Leave no background process behind, no `zed` and no fio.
- Never print the environment, a token or the fio key (`env`, `printenv`,
  `set`, `/proc/*/environ`, `$W/perf/key`).
- A production hypervisor sits underneath: builds run under `nice -n 10
  ionice -c3` with `make -j8`, never more; tests and fio run under `nice -n 10
  ionice -c3`; one thing at a time, never two blocks at once.
- From STEP 1 on, every block starts by sourcing `/var/log/zts-kc-1026/env.sh`,
  so that it works in a shell that keeps no state between commands.
- STEPS 3, 4, 6, 7 and 8 run for 5 to 45 minutes each. Run each block whole;
  if the shell cannot wait that long in the foreground, start the block in
  the background, follow `$L/runs.txt` or `$L/perf.txt`, and start the next
  block only once the block's own `STEP <n> done` or `STEP <n> stopped` line
  is in `$L/steps.txt`.
- **Any value other than the one written next to a check means stop**, unless
  the text says the value is recorded only. After any line that says `stop`,
  run nothing more but STEP 9 (it skips what must not run). A kernel panic,
  oops, VERIFY, ASSERT or hung task means stop and report, never reboot: the
  maintainer decides about the reboot.
- The session replies once, at the end, in its own transcript: the whole of
  `REPORT.txt`.

What earlier VM runs taught, and these steps already handle:

- `modinfo` is in `/usr/sbin`, which is not in `tigerblue77`'s PATH: every
  block puts `/usr/sbin:/sbin` first. This VM's umask is 0002; `env.sh` sets
  022, so that files are created as on the build machines.
- The runner prints `Test: <path> (run as root) [mm:ss] [RESULT]` with a
  timestamp before it, `Test (Linux): ...` for a section of `linux.run`
  (`dedup_legacy_gang`, `send_encrypted_files` run alone), and, for a test run
  alone with `-t`, the path **with** `.ksh`. Results are matched anywhere in
  the line, in all three forms, and every line is counted.
- In a regex passed to `awk -v`, a literal parenthesis is written `[(]`,
  never `\(`; the patterns below follow that.
- `zfs-tests.sh -d` must point at a mode 1777 directory (`$W/files`). Each run
  waits 2 s first: two runs started in one second would share a results
  directory. A test that writes nothing on stderr has no `merged` file: the
  failing lines are read from the runner's own `log`.
- On a failed test the ZTS callbacks print and then clear dmesg: kernel
  messages are read per run from `journalctl -k --since`, and the dmesg count
  is only a per-tree cross-check. Lines that are not findings are dropped
  before counting: the runner's `ZTS run <test>` markers (a test name such as
  `send-c_verify_ratio` would match `VERIFY`) and the
  `spa_import_progress_set_notes_impl` notes ("Verifying ...").
- **The stack depot.** With `slub_debug=FZPU` the kernel keeps an allocation
  stack for every slab object, and this boot's stack depot has been full
  since earlier today; its one-time warning is already in the journal (STEP 0
  counts it). If a "Stack depot reached limit capacity" warning appears again,
  it is not a ZFS finding. It is told apart by its shape: a `------------[ cut
  here ]------------` block whose message line is `Stack depot reached limit
  capacity` and whose `WARNING:` line is at `lib/stackdepot.c`, with a call
  trace that runs through `stack_depot_save`, `set_track` and the slab
  allocator; any `[spl]` or `[zfs]` frames below those are only the caller of
  an allocation. A ZFS finding instead names ZFS: `VERIFY3(...) failed` and
  `PANIC at <file>.c:<line>:<function>()` (SPL's assertions), a `BUG:` or
  `Oops` with its `RIP` in `[zfs]` or `[spl]`, a `WARNING:` at a `module/`
  path, or a task blocked in ZFS functions. `kfilter` (in `env.sh`) folds each
  stack depot block into one `DEPOT SPLAT (...)` line, counted apart and never
  a reason to stop.
- The trees were built without `--runstatedir`, so their `zed` would keep its
  pid file in `/usr/local/var/run/zed.pid`. None of the selected tests starts
  a `zed` or makes `/etc/zfs/vdev_id.conf` (only the events tests do): the
  leftovers check requires no `zed`, no pid file and no `vdev_id.conf` after
  every run. `zfs_load-key`'s setup starts a python3 HTTPS server and logs to
  `/tmp/ZTS-snakeoil.log`; its cleanup stops it and removes the log, and the
  leftovers check requires both gone.
- `srcversion` hashes only a module's C sources, so B-rel's equals B's and
  M-rel's equals M's: the guard also reads `zfs_flags` right after each load
  (4294932990 in a debug build, 0 in a release one) and the module's
  `coresize`, which must not change between loads of one tree.

## STEP 0: the VM must be free, and the builds finished

Run this and keep its output (it is also saved to `$L/step0.txt`):

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-kc-1026; L=/var/log/zts-kc-1026
{
id -un; uname -r; grep PRETTY_NAME /etc/os-release; nproc; umask
grep -m1 'model name' /proc/cpuinfo
echo "cpu flags: $(grep -o -w -E 'aes|pclmulqdq|avx2|sha_ni' /proc/cpuinfo | sort -u | xargs)"
echo "cmdline: $(grep -o 'slub_debug=[^ ]*' /proc/cmdline)"
free -m | sed -n 1,2p
echo "/var/tmp free: $(df -BG --output=avail /var/tmp | tail -1 | tr -d ' ')"
ls -d /lib/modules/$(uname -r)/build
lsmod | grep -E '^(zfs|spl) '
ls /sys/module | grep -xE 'zfs|spl'
ls -l /dev/zfs 2>&1
pgrep -af 'zfs-tests|test-runner|ztest|zloop|zfs\.sh|make|gcc|cc1|ksh|zdb|fio|bpftrace' |
    grep -vE '^[0-9]+ ([^ ]*/)?bash( |$)'
pgrep -ax 'zed|lt-zed'
tmux list-panes -a -F '#{session_name} #{pane_pid} #{pane_current_command}' 2>&1
sudo -n true && echo "passwordless sudo ok"
ls -l /etc/zfs/zpool.cache /etc/zfs/vdev_id.conf /usr/local/var/run/zed.pid /tmp/ZTS-snakeoil.log 2>&1
echo "loop: $(sudo -n losetup -a | xargs); dm: $(sudo -n dmsetup ls 2>&1 | xargs)"
sudo -n journalctl -k -n 1 --no-pager
echo "stack depot warnings this boot: $(sudo -n journalctl -k -b --no-pager | grep -c 'Stack depot reached limit capacity')"
echo "modprobe options: $(grep -rhsE '^options +(zfs|spl) ' /etc/modprobe.d /usr/lib/modprobe.d /run/modprobe.d | xargs)"
for c in ksh python3 git modinfo sha256sum timeout fio openssl xxh128sum bzcat mkfs.ext4 mpstat bpftrace; do
	echo "$c: $(command -v $c || echo absent)"
done
echo "W holds: $(LC_ALL=C ls -A $W | xargs)"
echo "L holds: $(LC_ALL=C ls -A $L | xargs)"
stat -c '%U %a %n' $W $L
} 2>&1 | tee $L/step0.txt
```

Expected: `tigerblue77`, a `6.12` kernel, `Debian GNU/Linux 13`, `16`, `0002`, the
CPU model, the CPU flags (recorded only), `cmdline: slub_debug=FZPU`, about
7 GiB total, at least `12G` free (20G when this was written), the kernel
headers' path, then nothing for the module, `/sys/module`, process and `zed`
lines and `No such file or directory` for `/dev/zfs`, only this session's tmux
pane, `passwordless sudo ok`, `/etc/zfs/zpool.cache` as a 0-byte file and `No
such file or directory` for the three others, the loop and dm devices
(recorded only: they are not ours, STEP 2 keeps them as the baseline), one
kernel line, `stack depot warnings this boot: 1`, `modprobe options:` with
nothing after it (recorded: `zfs.sh` loads through `modprobe`, so such options
apply to every load, the same for every tree; one that sets `zfs_flags` would
make STEP 3's guard fail), a path for `ksh`, `python3`,
`git`, `modinfo`, `sha256sum` and `timeout`, a path or `absent` for the others
(recorded), `W holds: B M repo`, and `L holds:` with `build-B.log` and
`build-M.log` and no `env.sh`, `runs.txt`, `steps.txt` or `REPORT.txt`; both
directories `tigerblue77`'s.

Stop at once, changing nothing, if: `id -un` is not `tigerblue77` or `sudo -n
true` fails; the system is not Debian 13 on a 6.12 kernel with its headers; a
zfs or spl module is loaded or `/dev/zfs` exists; a test, build or fio process
or any `zed` runs; a tmux pane other than this session's runs anything;
`/etc/zfs/vdev_id.conf`, the zed pid file or `/tmp/ZTS-snakeoil.log` exists;
`/var/tmp` has less than 12G free; `journalctl -k` prints no kernel line; one
of `ksh`, `python3`, `git`, `modinfo`, `sha256sum`, `timeout` is absent; `W
holds` is not `B M repo`; or `$L` already holds an `env.sh`, `runs.txt`,
`steps.txt` or `REPORT.txt` (this task started once already: report, do not
start again).

These are recorded only, with what they change:

- `fio` absent: `l2` is a SKIP on both trees and STEPS 6 to 8 do not run
  (nothing is installed);
- `openssl` absent: `zfs_load-key`'s setup fails and its 7 tests are SKIPs on
  both trees; `xxh128sum`, `bzcat` or `mkfs.ext4` absent: the tests that call
  them fail on both trees;
- `mpstat` absent: `%sys` comes from `/proc/stat` alone; `bpftrace` absent: no
  `hkdf_sha512()` counts;
- a stack depot count other than 1: recorded, it only says how often this
  boot's depot warned.

Then wait for the builds of B and M to have finished: no build process, and a
srcversion readable from each tree's `module/zfs.ko`. Both were reported done
at 12:59Z, so this normally returns at the first check:

```sh
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-kc-1026; L=/var/log/zts-kc-1026
for i in $(seq 27); do
	n=$(pgrep -af 'autogen|configure|make|cc1|gcc|objtool|modpost|libtool' |
	    grep -vE '^[0-9]+ ([^ ]*/)?bash( |$)' | grep -c .)
	sb=$(modinfo -F srcversion $W/B/module/zfs.ko 2>/dev/null)
	sm=$(modinfo -F srcversion $W/M/module/zfs.ko 2>/dev/null)
	if [ "$n" = 0 ] && [ -n "$sb" ] && [ -n "$sm" ] && [ -s $L/build-B.log ] &&
	    [ -s $L/build-M.log ]; then
		echo "builds finished (check $i): B $sb, M $sm"
		break
	fi
	echo "check $i $(date '+%T'): $n build processes, B '$sb', M '$sm'"
	if [ $i = 27 ]; then echo "builds not finished after 9 min"; else sleep 20; fi
done 2>&1 | tee -a $L/step0.txt
tail -n 2 $L/build-B.log $L/build-M.log | tee -a $L/step0.txt
```

Expected: `builds finished (check 1): B 89263B1C179E680CDDE56B0, M
8852710DB871A7587B9E722`, then the last two lines of each build log. If it
prints `builds not finished after 9 min`, run the block again, up to six times
in all (an hour); after that, stop. A build process still running with both
modules present is not finished.

## STEP 1: environment, helpers, and the trees as built

This block writes `env.sh` (variables and every helper of the later steps),
`count.py` (it reads a tree's runfiles the way the runner does and counts the
`Test:` lines a run prints), `table.py` (the fio table) and `list.txt` (the
41 runs), and makes the test files directory.

```sh
export PATH=/usr/sbin:/sbin:$PATH
L=/var/log/zts-kc-1026
cat > $L/env.sh <<'ENV'
umask 022
export PATH=/usr/sbin:/sbin:$PATH
W=/var/tmp/zts-kc-1026
L=/var/log/zts-kc-1026
P=$W/perf
HB=3239e81a1de2cc760a3c21e809b886fbe2b091e8
HM=a128b8a075b5b13c737e56914cd7043bd3e9ebf8
SRCV_B=89263B1C179E680CDDE56B0
SRCV_M=8852710DB871A7587B9E722
# zfs_flags right after a load (spa_misc.c): every flag but DPRINTF,
# SET_ERROR, INDIRECT_REMAP and RAIDZ_RECONSTRUCT in a debug build, 0 in a
# release one. srcversion hashes only the C sources, so it cannot tell B
# from B-rel; this can.
FLAGS_dbg=4294932990
FLAGS_rel=0
# Where the trees' zed keeps its pid file (built without --runstatedir).
ZEDPID=/usr/local/var/run/zed.pid
KRE='VERIFY|ASSERT|PANIC|BUG:|Oops|call trace|blocked for more than'
# A result line of the runner, "Test: <path> (run as root) [mm:ss] [RESULT]",
# or "Test (Linux): ..." for a section of linux.run.
TRE='Test( [(][^)]*[)])?: .* [(]run as [^)]*[)] +\[[0-9:]+\] \[[A-Z]+\]'
# The tunables the selected tests change and must restore, the salt
# rotation the change depends on, and zfs_flags.
TUN="l2arc_noprefetch l2arc_rebuild_blocks_min_l2size zfs_scan_legacy
zfs_scan_suspend_progress zfs_dedup_log_txg_max zfs_dedup_log_flush_entries_min
zfs_dedup_log_flush_entries_max metaslab_force_ganging metaslab_force_ganging_pct
zfs_bclone_enabled zfs_disable_ivset_guid_check zfs_key_max_salt_uses zfs_flags"
# Parameters that must read the same at every load of the fio part.
PP="zfs_key_max_salt_uses zfs_arc_max zfs_arc_min zfs_prefetch_disable
zfs_dbgmsg_enable zfs_flags icp_aes_impl icp_gcm_impl zfs_sha256_impl"
# The lines of a failed test worth quoting.
NRE='^ERROR: |cannot |unsupported|not supported|No such file|compatibility'
# fio read phases: measured runtime (s), and the per-job file size of each
# dataset (4 jobs: 2 GiB at 16k, 1 GiB at 4k).
RT=30
SZ_16k=512m
SZ_4k=256m
# Absolute paths, so that bpftrace -c can start fio; MPSTAT and BPF are
# empty when the tool is absent (it is never installed).
FIO=$(command -v fio); NICE=$(command -v nice); IONICE=$(command -v ionice)
MPSTAT=$(command -v mpstat); BPF=$(command -v bpftrace)

same() {   # same <what> <got> <expected>
	[ "$2" = "$3" ] && echo "ok: $1" || echo "MISMATCH: $1: $2, expected $3"
}

# Processes matching <pattern>, but no bash: the session's own shells
# (and their forks) carry the text of the block being run.
others() {   # others <pattern>
	pgrep -af "$1" | grep -vE '^[0-9]+ ([^ ]*/)?bash( |$)'
}

# Kernel log lines without what is known not to be a finding: the runner's
# "ZTS run" markers (test names), the import progress notes ("Verifying
# ..."), and any WARN splat of a full stack depot, folded into one line.
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
# dmesg). Returns 1, and leaves $L/PANIC, on a panic, VERIFY, ASSERT, BUG,
# oops or hung task.
kern_check() {   # kern_check <label> <since>
	sudo -n journalctl -k --since "$2" --no-pager -o short-iso > $L/kern-$1.txt 2>&1
	kfilter < $L/kern-$1.txt > $L/kernf-$1.txt
	echo "kernel $1: $(grep -ciE "$KRE" $L/kernf-$1.txt) suspicious lines," \
	    "$(grep -c '^DEPOT SPLAT' $L/kernf-$1.txt) stack depot splats" | tee -a $L/runs.txt
	grep -iE "$KRE" $L/kernf-$1.txt | head -20 | sed 's/^/  /' | tee -a $L/runs.txt
	if grep -qE 'PANIC|VERIFY|ASSERT|BUG:|Oops|blocked for more than' $L/kernf-$1.txt; then
		echo "$1: panic, assertion or hung task in the kernel log: stop" | tee -a $L/runs.txt
		touch $L/PANIC
		return 1
	fi
}

# What dmesg still holds: a per-tree count against the baseline.
dmesg_state() {   # dmesg_state <label>
	sudo -n dmesg --time-format=iso 2>&1 | kfilter > $L/dmesg-$1.txt
	echo "dmesg $1: $(grep -ciE "$KRE" $L/dmesg-$1.txt) suspicious lines in the buffer" \
	    "(baseline $(cat $L/dmesg-baseline.txt); a failed test clears it)" | tee -a $L/runs.txt
}

# Load <tree>'s modules, <kind> dbg or rel. Whatever is loaded must be
# <prev>'s ("none" for the first load) and is unloaded first. The guard:
# the module files are the ones STEP 1 or 6 hashed, the loaded srcversion
# is the tree's, zfs_flags says debug or release, and the module's size
# matches every earlier load of the same tree. No pool may appear.
switch_to() {   # switch_to <tree> <prev> <dbg|rel>
	local cur p f t
	cur=$(cat /sys/module/zfs/srcversion 2>/dev/null)
	if [ -n "$cur" ]; then
		if [ "$2" = none ] || [ "$cur" != "$(cat $L/srcv-$2)" ] ||
		    [ "$(cat $L/loaded-tree 2>/dev/null)" != "$2" ]; then
			echo "loaded zfs module $cur is not $2's: stop" | tee -a $L/runs.txt
			return 1
		fi
		(cd $W/$2 && sudo -n ./scripts/zfs.sh -u) > $L/unload.log 2>&1
		p=$?
		echo "unload $2 rc=$p" | tee -a $L/runs.txt
		[ $p -eq 0 ] || { echo "unload failed: stop" | tee -a $L/runs.txt; return 1; }
		rm -f $L/loaded-tree $L/loaded-srcv
	fi
	if lsmod | grep -qE '^(zfs|spl) '; then
		echo "zfs or spl still loaded: stop" | tee -a $L/runs.txt
		return 1
	fi
	if ! (cd $W/$1 && sha256sum -c --quiet $L/ko-$1.sha256); then
		echo "$1: module files differ from their record: stop" | tee -a $L/runs.txt
		return 1
	fi
	(cd $W/$1 && sudo -n ./scripts/zfs.sh) > $L/load.log 2>&1
	echo "load $1 rc=$?" | tee -a $L/runs.txt
	f=$(cat /sys/module/zfs/parameters/zfs_flags 2>&1)
	p=$(cat /sys/module/zfs/coresize 2>&1)
	if [ "$(cat /sys/module/zfs/srcversion 2>&1)" != "$(cat $L/srcv-$1)" ] ||
	    [ "$(modinfo -F srcversion $W/$1/module/zfs.ko)" != "$(cat $L/srcv-$1)" ] ||
	    [ "$f" != "$(eval echo \$FLAGS_$3)" ]; then
		echo "$1 GUARD FAILED: srcversion $(cat /sys/module/zfs/srcversion 2>&1)," \
		    "zfs_flags $f: stop" | tee -a $L/runs.txt
		return 1
	fi
	[ -s $L/coresize-$1 ] || echo "$p" > $L/coresize-$1
	if [ "$p" != "$(cat $L/coresize-$1)" ]; then
		echo "$1 GUARD FAILED: coresize $p, earlier $(cat $L/coresize-$1): stop" | tee -a $L/runs.txt
		return 1
	fi
	cat $L/srcv-$1 > $L/loaded-srcv
	echo $1 > $L/loaded-tree
	for t in $TUN; do echo "$t=$(cat /sys/module/zfs/parameters/$t 2>&1)"; done > $L/tunables-loaded.txt
	if grep -q 'No such file' $L/tunables-loaded.txt; then
		echo "$1: a tunable is missing: stop" | tee -a $L/runs.txt
		return 1
	fi
	echo "$1 guard ok: srcversion $(cat $L/loaded-srcv), zfs_flags $f, coresize $p," \
	    "spl_panic_halt=$(cat /sys/module/spl/parameters/spl_panic_halt)" | tee -a $L/runs.txt
	p=$(cd $W/$1 && sudo -n ./zpool list -H -o name 2>&1)
	if [ -n "$p" ]; then
		echo "$1: pools present right after the load: $p: stop" | tee -a $L/runs.txt
		touch $L/HELD
		return 1
	fi
	echo "$1: no pool after the load" | tee -a $L/runs.txt
}

# Before any run: the loaded module is still the one switch_to checked.
pre_run() {   # pre_run <label>
	if [ "$(cat /sys/module/zfs/srcversion 2>&1)" != "$(cat $L/loaded-srcv 2>&1)" ] ||
	    [ "$PWD" != "$W/$(cat $L/loaded-tree 2>&1)" ]; then
		echo "$1: loaded module or working directory is not $(cat $L/loaded-tree 2>&1)'s: stop" |
		    tee -a $L/runs.txt
		return 1
	fi
}

# Leftovers of a run, from the root of the loaded tree. Pools, devices,
# mounts and processes may hold the module (HELD); files and tunables are
# the tests' to restore. All of them stop. New entries in / are noted.
leftovers() {   # leftovers <label>
	local bad=0 p f t
	{
	p=$(sudo -n ./zpool list -H -o name 2>&1)
	[ -n "$p" ] && { echo "  LEFTOVER pools: $p"; bad=1; touch $L/HELD; }
	sudo -n losetup -a | diff -q $L/losetup-before.txt - > /dev/null ||
	    { echo "  LEFTOVER loop devices:"; sudo -n losetup -a; bad=1; touch $L/HELD; }
	sudo -n dmsetup ls 2>&1 | diff -q $L/dmsetup-before.txt - > /dev/null ||
	    { echo "  LEFTOVER dm devices:"; sudo -n dmsetup ls; bad=1; touch $L/HELD; }
	grep -w zfs /proc/mounts && { bad=1; touch $L/HELD; }
	p=$(others 'test-runner\.py|zfs-tests\.sh|zfs-tests/tests/functional|zdb |http\.server|fio |bpftrace|mpstat')
	[ -n "$p" ] && { echo "  LEFTOVER processes:"; echo "$p"; bad=1; touch $L/HELD; }
	p=$(pgrep -ax 'zed|lt-zed')
	[ -n "$p" ] && { echo "  LEFTOVER zed:"; echo "$p"; bad=1; touch $L/HELD; }
	for f in /etc/zfs/vdev_id.conf $ZEDPID /tmp/ZTS-snakeoil.log; do
		if [ -e $f ] || [ -L $f ]; then echo "  LEFTOVER $(ls -l $f)"; bad=1; fi
	done
	f=$(ls -A $W/files)
	if echo "$f" | grep -qE '^(file-vdev|constrained_path|zfs-tests\.|zts-|testdir|backdir|disk\.l2arc)'; then
		echo "  LEFTOVER files in $W/files:" $f; bad=1
	elif [ -n "$f" ]; then
		echo "  other files in $W/files (not fatal):" $f
	fi
	for t in $TUN; do echo "$t=$(cat /sys/module/zfs/parameters/$t 2>&1)"; done |
	    diff $L/tunables-loaded.txt - | grep '^>' | sed 's/^>/  LEFTOVER tunable:/' | grep . && bad=1
	ls -A / | diff $L/root-before.txt - | grep '^[<>]' | sed 's/^/  entry of \/ changed (not fatal): /'
	echo "  zpool.cache: $(stat -c '%s bytes' /etc/zfs/zpool.cache 2>/dev/null || echo absent)"
	echo "leftovers after $1: $([ $bad = 0 ] && echo none || echo 'FOUND: stop')"
	} > $L/left-$1.txt 2>&1
	tee -a $L/runs.txt < $L/left-$1.txt
	return $bad
}

# Result counts of runner Test: lines (stdin), e.g. "PASS x3" or
# "PASS x18, SKIP x1".
tally() {
	grep -oE '(PASS|FAIL|SKIP|KILLED)\]( [(]RERAN[)])?$' | sed 's/\].*$//' |
	    sort | uniq -c | awk '{ printf "%s%s x%d", s, $2, $1; s = ", " }'
}

# "<RESULT> <dir/test>" for each Test: line (stdin) that did not pass (a -t
# run names its test with .ksh, a group without: the suffix is dropped).
notpassed() {
	grep -v '\[PASS\]' | sed -nE \
	    's|^.*Test( [(][^)]*[)])?: .*/tests/functional/([^ ]+) [(]run as [^)]*[)] +\[[0-9:]+\] \[([A-Z]+)\].*$|\3 \2|p' |
	    sed 's/\.ksh$//'
}

# For each run of test <name> that did not pass, from the runner's log:
# its Test: line, the lines matching NRE before the failure callbacks, the
# line just before them, and the first ERROR line.
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

# The end of every run: kernel messages, outer timeout, leftovers.
post_run() {   # post_run <label> <since> <rc>
	kern_check $1 "$2" || return 1
	if [ $3 -eq 124 ] || [ $3 -eq 137 ]; then
		echo "$1: outer timeout: stop" | tee -a $L/runs.txt
		return 1
	fi
	leftovers $1
}

# One zfs-tests.sh run from the root of the loaded tree: <how> is -t with a
# test (<arg> = dir/test under tests/functional) or -T with tags. It runs
# as this user under nice, ionice and an outer timeout; then its counts
# against <expected> Test: lines, every line that did not pass with the
# digest of its log, the kernel messages and the leftovers. Returns 1 when
# nothing more may run.
zts() {   # zts <label> <-t|-T> <arg> <expected lines>
	local label=$1 since t0 rc d n c a r t
	pre_run $label || return 1
	sleep 2   # two runs started in one second would share a results directory
	[ "$2" = -t ] && a=tests/functional/$3.ksh || a=$3
	since=$(date '+%F %T')
	t0=$(date +%s)
	timeout -k 60 $([ "$2" = -t ] && echo 1800 || echo 3600) nice -n 10 ionice -c3 \
	    ./scripts/zfs-tests.sh -vK -d $W/files $2 $a < /dev/null > $L/$label.log 2>&1
	rc=$?
	c=$(grep -E "$TRE" $L/$label.log | tally)
	n=$(grep -cE "$TRE" $L/$label.log)
	echo "$label $2 $3 ${c:-NO RESULT} ($n of $4 lines) rc=$rc wall=$(( $(date +%s) - t0 ))s" |
	    tee -a $L/runs.txt
	echo "$label ${c:-NO RESULT} lines=$n/$4 rc=$rc" >> $L/zts-results.txt
	grep -E "$TRE" $L/$label.log | notpassed > $L/$label.notpassed
	d=$(awk '/^Log directory/ {print $3}' $L/$label.log | head -1)
	while read -r -u 4 r t; do
		if [ -f "$d/log" ]; then
			log_digest "$d" "${t##*/}" | tee -a $L/runs.txt
		else
			echo "  $r $t: no runner log (Log directory: '$d')" | tee -a $L/runs.txt
		fi
	done 4< $L/$label.notpassed
	post_run $label "$since" $rc || return 1
	if [ "$n" != "$4" ]; then
		echo "$label: $n result lines, expected $4: stop" | tee -a $L/runs.txt
		return 1
	fi
}

# The whole list on the loaded tree, labels prefixed with <p>.
run_list() {   # run_list <p>
	local label how arg n
	while read -r -u 3 label how arg n; do
		case $label in '#'*|'') continue ;; esac
		zts $1-$label $how $arg $n || return 1
	done 3< $L/list.txt
	echo "list done on $(cat $L/loaded-tree)" | tee -a $L/runs.txt
}

# On M: once more, alone, each test that FAILED or was KILLED on B (a
# group whose setup or cleanup failed runs again as a group). At most 12.
rerun_b_failures() {
	local label how arg n t i=0
	while read -r -u 3 label how arg n; do
		case $label in '#'*|'') continue ;; esac
		[ -s $L/b-$label.notpassed ] || continue
		if [ $how = -t ] || grep -qE '^(FAIL|KILLED) .*/(setup|cleanup)$' $L/b-$label.notpassed; then
			grep -qE '^(FAIL|KILLED) ' $L/b-$label.notpassed || continue
			i=$((i + 1)); [ $i -le 12 ] || { echo "mr-$label: not rerun (more than 12)" | tee -a $L/runs.txt; continue; }
			zts mr-$label $how $arg $n || return 1
			continue
		fi
		for t in $(awk '$1 ~ /^(FAIL|KILLED)$/ {print $2}' $L/b-$label.notpassed); do
			i=$((i + 1)); [ $i -le 12 ] || { echo "mr-$label ($t): not rerun (more than 12)" | tee -a $L/runs.txt; continue; }
			zts mr-$label-$i -t $t "$(python3 -I $L/count.py $W/M -t $t | cut -d' ' -f2)" || return 1
		done
	done 3< $L/list.txt
	echo "reruns on M: $i" | tee -a $L/runs.txt
}

# B against M, label by label: the results, and the tests that did not
# pass on B only, on both, on M only. A test failing on both is master's
# or this VM's; one failing on B only is the branch's until shown otherwise.
compare_zts() {
	local label how arg n b m
	while read -r -u 3 label how arg n; do
		case $label in '#'*|'') continue ;; esac
		printf '%-4s %-44s B: %-24s M: %s\n' $label $arg \
		    "$(sed -n "s/^b-$label \(.*\) lines=.*$/\1/p" $L/zts-results.txt)" \
		    "$(sed -n "s/^m-$label \(.*\) lines=.*$/\1/p" $L/zts-results.txt)"
		b=$(LC_ALL=C sort -k2 $L/b-$label.notpassed 2>/dev/null)
		m=$(LC_ALL=C sort -k2 $L/m-$label.notpassed 2>/dev/null)
		LC_ALL=C join -j 2 -o 0,1.1,2.1 <(echo "$b") <(echo "$m") | grep "[^ ]" |
		    sed 's/^/       on both: /'
		LC_ALL=C join -j 2 -v 1 -o 0,1.1 <(echo "$b") <(echo "$m") | grep "[^ ]" |
		    sed 's/^/       B ONLY: /'
		LC_ALL=C join -j 2 -v 2 -o 0,2.1 <(echo "$b") <(echo "$m") | grep "[^ ]" |
		    sed 's/^/       M only: /'
	done 3< $L/list.txt
	grep '^mr-' $L/zts-results.txt | sed 's/^/rerun on M: /'
}

# Build release tree <tree> (configure without --enable-debug), niced, and
# record it as STEP 1 recorded B and M; <twin> is the debug tree of the
# same commit.
build_rel() {   # build_rel <tree> <twin>
	local t0 rc
	cd $W/$1 || return 1
	t0=$(date +%s)
	nice -n 10 ionice -c3 sh -c './autogen.sh && ./configure && make -j8' \
	    > $L/build-$1.log 2>&1
	rc=$?
	echo "build $1 rc=$rc wall=$(( $(date +%s) - t0 ))s" | tee -a $L/perf.txt
	if [ $rc -ne 0 ]; then
		grep -nE ': error:|\*\*\* \[' $L/build-$1.log | head -20 | tee -a $L/perf.txt
		echo "build $1 failed: stop" | tee -a $L/perf.txt
		return 1
	fi
	{
	same "$1 head" "$(git rev-parse HEAD)" "$(git -C $W/$2 rev-parse HEAD)"
	same "$1 tracked changes" "$(git diff HEAD --name-only | wc -l)" 0
	same "$1 is a release build (ZFS_DEBUG lines)" "$(grep -c '^#define ZFS_DEBUG 1$' zfs_config.h)" 1
	same "$1 backend: ICP (no kcapi)" "$(grep -c '^#define HAVE_KERNEL_CRYPTO 1$' zfs_config.h)" 0
	same "$1 build errors" "$(grep -cE ': error:|\*\*\* \[' $L/build-$1.log)" 0
	} | tee -a $L/perf.txt
	echo "$1 compiler warnings: $(grep -cE ':[0-9]+:[0-9]+: warning:' $L/build-$1.log)," \
	    "objtool notes: $(grep -c 'warning: objtool:' $L/build-$1.log)" | tee -a $L/perf.txt
	modinfo -F srcversion module/zfs.ko > $L/srcv-$1
	sha256sum module/zfs.ko module/spl.ko > $L/ko-$1.sha256
	echo "$1 srcversion $(cat $L/srcv-$1) ($2: $(cat $L/srcv-$2))" | tee -a $L/perf.txt
	! grep -q "^MISMATCH: $1 " $L/perf.txt
}

# The fio part. One read job of <runtime> s on <dir>, as one line, with
# absolute paths so that bpftrace -c can run it.
fio_cmd() {   # fio_cmd <dir> <bs> <size per job> <runtime> <json>
	echo "$NICE -n 10 $IONICE -c3 $FIO --name=kc --directory=$1 --rw=randread --bs=$2" \
	    "--size=$3 --numjobs=4 --iodepth=32 --ioengine=libaio --time_based --runtime=$4" \
	    "--group_reporting --allow_file_create=0 --output-format=json --output=$5"
}

# IOPS, reads, mean latency (us) and error of a fio JSON file.
fio_json() {   # fio_json <json>
	python3 -I -c 'import json, sys
j = json.load(open(sys.argv[1]))["jobs"][0]
print("%d %d %.1f %d" % (round(j["read"]["iops"]), j["read"]["total_ios"],
      j["read"]["lat_ns"]["mean"] / 1000, j["error"]))' "$1" 2>/dev/null || echo "0 0 0 1"
}

# %sys, %usr (+nice), %steal and %idle (+iowait) between two "cpu " lines
# of /proc/stat: the counters mpstat reads, over all CPUs.
cpu_pct() {   # cpu_pct "<before>" "<after>"
	echo "$1 $2" | awk '{ for (i = 2; i <= 9; i++) { d[i] = $(i + 11) - $i; t += d[i] }
	    printf "%.1f %.1f %.1f %.1f", 100 * d[4] / t, 100 * (d[2] + d[3]) / t,
	    100 * d[9] / t, 100 * (d[5] + d[6]) / t }'
}

# One read phase of dataset <ds> on the loaded release tree: a 10 s warm-up
# (it fills the ARC with the metadata and lets B admit the salt), under
# bpftrace's hkdf_sha512 count when bpftrace is present, then the measured
# RT seconds with the CPU counters, mpstat and the page-ins around it.
perf_read() {   # perf_read <label> <ds> <bs> <size per job>
	local l=$1 d=$P/mnt/$2 rc s0 s1 g0 g1 mpid= h=n/a w ms=n/a x
	if [ -n "$BPF" ]; then
		sudo -n $BPF -e 'kprobe:hkdf_sha512 { @n = count(); }' \
		    -c "$(fio_cmd $d $3 $4 10 $L/perf/warm-$l.json)" > $L/perf/bpf-$l.txt 2>&1
		if [ $? -eq 0 ] && [ -s $L/perf/warm-$l.json ]; then
			h=$(sed -n 's/^@n: //p' $L/perf/bpf-$l.txt); h=${h:-0}
		fi
	fi
	if [ ! -s $L/perf/warm-$l.json ]; then
		$(fio_cmd $d $3 $4 10 $L/perf/warm-$l.json) < /dev/null > $L/perf/warm-$l.err 2>&1
	fi
	w=$(fio_json $L/perf/warm-$l.json)
	[ "${w##* }" = 0 ] || { echo "$l: warm-up failed ($w): stop" | tee -a $L/perf.txt; return 1; }
	s0=$(grep '^cpu ' /proc/stat); g0=$(awk '$1 == "pgpgin" {print $2}' /proc/vmstat)
	if [ -n "$MPSTAT" ]; then
		LC_ALL=C $MPSTAT -P ALL 1 $RT > $L/perf/mpstat-$l.txt 2>&1 &
		mpid=$!
	fi
	$(fio_cmd $d $3 $4 $RT $L/perf/fio-$l.json) < /dev/null > $L/perf/fio-$l.err 2>&1
	rc=$?
	[ -n "$mpid" ] && wait $mpid
	s1=$(grep '^cpu ' /proc/stat); g1=$(awk '$1 == "pgpgin" {print $2}' /proc/vmstat)
	x=$(fio_json $L/perf/fio-$l.json)
	if [ $rc -ne 0 ] || [ "${x##* }" != 0 ]; then
		echo "$l: fio rc=$rc ($x): stop" | tee -a $L/perf.txt
		return 1
	fi
	[ -n "$MPSTAT" ] && ms=$(awk '$1 == "Average:" && $2 == "CPU" { for (i = 1; i <= NF; i++) if ($i == "%sys") c = i }
	    $1 == "Average:" && $2 == "all" && c { print $c }' $L/perf/mpstat-$l.txt)
	set -- $l $x $(cpu_pct "$s0" "$s1") $((g1 - g0)) $w
	# $1 label, $2 iops, $3 reads, $4 lat_us, $5 err, $6 sys, $7 usr, $8 steal,
	# $9 idle, $10 pgpgin KiB, $11 warm iops, $12 warm reads
	echo "$1 IOPS $2 reads $3 lat ${4}us sys $6% usr $7% steal $8% idle $9%" \
	    "mpstat-sys ${ms:-n/a}% pgpgin ${10}KiB warm-reads ${12} hkdf $h" | tee -a $L/perf.txt
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' $1 $2 $6 $7 $8 ${ms:-n/a} ${10} ${12} $h $3 \
	    >> $L/perf/results.tsv
}

# Import the pool on the loaded release tree, load both keys (each load
# picks a new current salt, so every block on disk is "older salt"),
# mount, and check what every round must find. One ok or MISMATCH line.
perf_up() {   # perf_up <label>
	local ds x e=
	sudo -n ./zpool import -d $P/vdev -o cachefile=none kcperf || e="$e import"
	for ds in r16k r4k; do
		sudo -n ./zfs load-key kcperf/$ds > /dev/null || e="$e load-key-$ds"
		sudo -n ./zfs mount kcperf/$ds || e="$e mount-$ds"
	done
	[ "$(sudo -n ./zpool list -H -o name | xargs)" = kcperf ] || e="$e pools"
	x=$(sudo -n ./zfs get -H -o value keystatus,mounted kcperf/r16k kcperf/r4k | xargs)
	[ "$x" = "available yes available yes" ] || e="$e keys/mounts($x)"
	x=$(stat -c %s $P/mnt/r16k/kc.[0-3].0 $P/mnt/r4k/kc.[0-3].0 | sort | uniq -c | xargs)
	[ "$x" = "4 268435456 4 536870912" ] || e="$e files($x)"
	for x in $PP; do echo "$x=$(cat /sys/module/zfs/parameters/$x)"; done > $L/perf/params-$1.txt
	[ -s $L/perf/params-first.txt ] || cp $L/perf/params-$1.txt $L/perf/params-first.txt
	cmp -s $L/perf/params-first.txt $L/perf/params-$1.txt || e="$e parameters"
	[ -z "$e" ] && echo "ok: $1 up: pool, keys, mounts, files, parameters" ||
	    echo "MISMATCH: $1 up:$e"
}

perf_down() {   # perf_down <label>
	local e=
	sudo -n ./zpool export kcperf || e="$e export"
	[ "$(sudo -n ./zpool list -H -o name | wc -l)" = 0 ] || e="$e pools"
	grep -qw zfs /proc/mounts && e="$e mounts"
	[ -z "$(others 'fio |bpftrace|mpstat')" ] || e="$e processes"
	[ -z "$e" ] && echo "ok: $1 down: exported, no pool, mount or process left" ||
	    echo "MISMATCH: $1 down:$e"
}

# STEP 7, on M-rel: the pool (one sparse 8 GiB file vdev under $P, no
# cache file, mounted on the empty $P/mnt: zpool create refuses a
# directory with entries), two encryption roots, one per recordsize,
# written once by
# fio; then VM-NOTE's unload and load of each key, so that every block on
# disk is written under an older salt; then M-rel reads everything back
# (the reference hash, and the page cache of the vdev file filled).
perf_prep() {
	local rs sz t0
	cd $W/M-rel || return 1
	mkdir -p $P/vdev $P/mnt && truncate -s 8G $P/vdev/vdev0 &&
	    dd if=/dev/urandom of=$P/key bs=32 count=1 status=none && chmod 600 $P/key || return 1
	sudo -n ./zpool create -o cachefile=none -m $P/mnt kcperf $P/vdev/vdev0 || return 1
	for rs in 16k 4k; do
		sudo -n ./zfs create -o encryption=on -o keyformat=raw \
		    -o keylocation=file://$P/key -o recordsize=$rs -o primarycache=metadata \
		    -o compression=off -o atime=off kcperf/r$rs || return 1
		sudo -n chown "$(id -un):" $P/mnt/r$rs || return 1
	done
	sudo -n ./zfs get -H -o name,property,value \
	    encryption,keyformat,encryptionroot,recordsize,primarycache,compression,atime,checksum \
	    kcperf/r16k kcperf/r4k | tee -a $L/perf.txt
	for rs in 16k 4k; do
		eval sz=\$SZ_$rs
		t0=$(date +%s)
		$NICE -n 10 $IONICE -c3 $FIO --name=kc --directory=$P/mnt/r$rs --rw=write --bs=$rs \
		    --size=$sz --numjobs=4 --end_fsync=1 --group_reporting --output-format=json \
		    --output=$L/perf/write-r$rs.json < /dev/null > $L/perf/write-r$rs.err 2>&1 ||
		    { echo "write r$rs: fio failed: stop" | tee -a $L/perf.txt; return 1; }
		echo "write r$rs: $(python3 -I -c 'import json, sys
j = json.load(open(sys.argv[1]))["jobs"][0]
print("%d MiB, %d KiB/s, error %d" % (j["write"]["io_kbytes"] / 1024, j["write"]["bw"], j["error"]))' \
		    $L/perf/write-r$rs.json), wall $(( $(date +%s) - t0 ))s" | tee -a $L/perf.txt
	done
	for rs in 16k 4k; do
		sudo -n ./zfs unmount kcperf/r$rs && sudo -n ./zfs unload-key kcperf/r$rs &&
		    sudo -n ./zfs load-key kcperf/r$rs > /dev/null && sudo -n ./zfs mount kcperf/r$rs ||
		    { echo "key reload of r$rs failed: stop" | tee -a $L/perf.txt; return 1; }
	done
	echo "keys unloaded and loaded again: every block is now under an older salt" | tee -a $L/perf.txt
	t0=$(date +%s)
	perf_hash prep M-rel
	echo "reference hashes: r16k $(cut -c1-16 $L/perf/hash-r16k-M-rel.txt)...," \
	    "r4k $(cut -c1-16 $L/perf/hash-r4k-M-rel.txt)..., read in $(( $(date +%s) - t0 ))s" |
	    tee -a $L/perf.txt
	echo "space: $(sudo -n ./zfs get -Hp -o name,value used kcperf/r16k kcperf/r4k | xargs)," \
	    "vdev file $(du -m $P/vdev/vdev0 | cut -f1) MiB on disk" | tee -a $L/perf.txt
	perf_down prep | tee -a $L/perf.txt
	! grep -q '^MISMATCH: prep ' $L/perf.txt
}

# Five rounds, M-rel then B-rel in each, every one on a fresh module load.
perf_rounds() {
	local r t prev=M-rel since
	for r in 1 2 3 4 5; do
		for t in M-rel B-rel; do
			since=$(date '+%F %T')
			echo "== round $r $t $(date '+%T') load $(cut -d' ' -f1-3 /proc/loadavg)" | tee -a $L/perf.txt
			switch_to $t $prev rel || return 1
			prev=$t
			cd $W/$t || return 1
			perf_up $r-$t | tee -a $L/perf.txt
			grep -q "^MISMATCH: $r-$t " $L/perf.txt && { echo "$r-$t: stop" | tee -a $L/perf.txt; return 1; }
			perf_read $r-$t-16k r16k 16k $SZ_16k || return 1
			perf_read $r-$t-4k r4k 4k $SZ_4k || return 1
			if [ $r = 5 ] && [ $t = B-rel ]; then
				perf_hash $r-$t B-rel | tee -a $L/perf.txt
			fi
			perf_down $r-$t | tee -a $L/perf.txt
			grep -q "^MISMATCH: $r-$t " $L/perf.txt && { echo "$r-$t: stop" | tee -a $L/perf.txt; return 1; }
			kern_check perf-$r-$t "$since" || return 1
		done
	done
	echo "perf rounds done" | tee -a $L/perf.txt
}

# sha256 of each dataset's four files, read back through the loaded tree
# (<tree>); B-rel's must equal M-rel's from STEP 7.
perf_hash() {   # perf_hash <label> <tree>
	local ds
	for ds in r16k r4k; do
		cat $P/mnt/$ds/kc.0.0 $P/mnt/$ds/kc.1.0 $P/mnt/$ds/kc.2.0 $P/mnt/$ds/kc.3.0 |
		    sha256sum | cut -d' ' -f1 > $L/perf/hash-$ds-$2.txt
		[ "$2" = M-rel ] && continue
		same "$1 $ds read back by $2 as M-rel read it" "$(cat $L/perf/hash-$ds-$2.txt)" \
		    "$(cat $L/perf/hash-$ds-M-rel.txt)"
	done
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
# table.py <results.tsv> <cpus> <runtime s>: median [min-max] per cell.
# Columns: label iops sys usr steal mpstat-sys pgpgin-KiB warm-reads hkdf reads
rows = [l.rstrip('\n').split('\t') for l in open(sys.argv[1]) if l.strip()]
cpus, rt = int(sys.argv[2]), float(sys.argv[3])
cells = {}
for r in rows:
    lab = r[0].split('-')
    rnd, rs, tree = lab[0], lab[-1], '-'.join(lab[1:-1])
    iops, sys_, usr, steal = float(r[1]), float(r[2]), float(r[3]), float(r[4])
    reads = float(r[9])
    # CPU time of the whole VM per read, in microseconds
    cpu_us = (sys_ + usr) / 100 * cpus * rt * 1e6 / reads if reads else 0
    h = r[8]
    hk = float(h) / float(r[7]) if h != 'n/a' and float(r[7]) else None
    cells.setdefault((rs, tree), []).append(
        dict(rnd=rnd, iops=iops, sys=sys_, steal=steal, cpu=cpu_us,
             pg=float(r[6]), hk=hk, hraw=h, mp=r[5]))


def mr(v, f='%.0f'):
    return (f + ' [' + f + '-' + f + ']') % (statistics.median(v), min(v), max(v))


print('%-4s %-6s %-2s %-24s %-20s %-20s %-7s %-9s %s' % (
    'rs', 'tree', 'n', 'IOPS', '%sys', 'CPU us/read', '%steal', 'pgpgin', 'hkdf/read (warm-up)'))
med = {}
for rs in ('16k', '4k'):
    for tree in ('M-rel', 'B-rel'):
        c = cells.get((rs, tree), [])
        if not c:
            print('%-4s %-6s none' % (rs, tree))
            continue
        hk = [x['hk'] for x in c if x['hk'] is not None]
        med[(rs, tree)] = (statistics.median([x['iops'] for x in c]),
                           statistics.median([x['cpu'] for x in c]),
                           statistics.median([x['sys'] for x in c]))
        print('%-4s %-6s %-2d %-24s %-20s %-20s %-7s %-9s %s' % (
            rs, tree, len(c), mr([x['iops'] for x in c]),
            mr([x['sys'] for x in c], '%.1f'), mr([x['cpu'] for x in c], '%.1f'),
            '%.1f' % max(x['steal'] for x in c), '%.0f' % max(x['pg'] for x in c),
            ('%.4f' % statistics.median(hk)) if hk else 'n/a'))
    if (rs, 'M-rel') in med and (rs, 'B-rel') in med:
        m, b = med[(rs, 'M-rel')], med[(rs, 'B-rel')]
        print('%-4s B/M    IOPS %+.1f%%, CPU per read %+.1f%%, %%sys %+.1f points (medians)' % (
            rs, 100 * (b[0] / m[0] - 1), 100 * (b[1] / m[1] - 1), b[2] - m[2]))
print('per round (IOPS M-rel/B-rel): ' + '; '.join(
    '%s %s: %s' % (rs, x['rnd'], '/'.join(
        '%.0f' % y['iops'] for t in ('M-rel', 'B-rel')
        for y in cells.get((rs, t), []) if y['rnd'] == x['rnd']))
    for rs in ('16k', '4k') for x in cells.get((rs, 'M-rel'), [])))
print('hkdf calls in each warm-up (M-rel, B-rel): ' + '; '.join(
    '%s: %s' % (rs, ' '.join(x['hraw'] for t in ('M-rel', 'B-rel')
                             for x in cells.get((rs, t), [])))
    for rs in ('16k', '4k')))
PY
cat > $L/list.txt <<'LIST'
# label how argument expected-Test:-lines; how is -T (tags) or -t (a test,
# as dir/test under tests/functional, run alone with its setup and cleanup)
kg  -T zfs_load-key,zfs_unload-key,zfs_change-key 26
r01 -t rsend/recv_dedup_encrypted_zvol 3
r02 -t rsend/send-wR_encrypted_zvol 3
r03 -t rsend/send_encrypted_files 3
r04 -t rsend/send_encrypted_freeobjects 3
r05 -t rsend/send_encrypted_hierarchy 3
r06 -t rsend/send_encrypted_holds 3
r07 -t rsend/send_encrypted_incremental 3
r08 -t rsend/send_encrypted_props 3
r09 -t rsend/send_encrypted_truncated_files 3
r10 -t rsend/send_leak_keymaps 3
r11 -t rsend/send_mixed_raw 3
r12 -t rsend/send_raw_ashift 3
r13 -t rsend/send_raw_large_blocks 3
r14 -t rsend/send_raw_spill_block 3
r15 -t rsend/send_realloc_encrypted_files 3
c01 -t cli_root/zfs_receive/zfs_receive_from_encrypted 3
c02 -t cli_root/zfs_receive/zfs_receive_to_encrypted 3
c03 -t cli_root/zfs_receive/zfs_receive_raw 3
c04 -t cli_root/zfs_receive/zfs_receive_raw_incremental 3
c05 -t cli_root/zfs_receive/zfs_receive_raw_-d 3
c06 -t cli_root/zfs_receive/zfs_receive_raw_-b 3
c07 -t cli_root/zfs_receive/zfs_receive_-wR-encrypted-mix 3
c08 -t cli_root/zfs_receive/zfs_receive_corrective 3
c09 -t cli_root/zfs_receive/zfs_receive_compressed_corrective 3
c10 -t cli_root/zfs_receive/zfs_receive_large_block_corrective 3
l2  -t l2arc/persist_l2arc_002_pos 3
s1  -t cli_root/zpool_scrub/zpool_scrub_encrypted_unloaded 3
s2  -t cli_root/zpool_scrub/zpool_scrub_thorough 3
i1  -t cli_root/zpool_import/zpool_import_encrypted 3
i2  -t cli_root/zpool_import/zpool_import_encrypted_load 3
i3  -t cli_root/zpool_import/zpool_import_encrypted_zplprops 3
i4  -t cli_root/zpool_import/zpool_import_errata3 3
i5  -t cli_root/zpool_import/zpool_import_errata4 3
k1  -t block_cloning/block_cloning_cross_enc_dataset 3
k2  -t block_cloning/block_cloning_replay_encrypted 3
z1  -t cli_root/zdb/zdb_encrypted 1
z2  -t cli_root/zdb/zdb_encrypted_raw 1
xg  -T send_xdr_encoding 16
sg  -T zfs_send 15
dg  -T dedup 19
LIST
. $L/env.sh
mkdir -m 1777 $W/files && mkdir -p $L/results $L/perf &&
    echo "files dir mode $(stat -c %a $W/files); $(grep -cvE '^(#|$)' $L/list.txt) runs listed"
for f in env.sh count.py table.py list.txt; do
	echo "$f: sha256 $(sha256sum < $L/$f | cut -d' ' -f1)," \
	    "without blanks $(tr -d ' \t' < $L/$f | sha256sum | cut -c1-16)"
done | tee $L/files.txt
```

Expected: `files dir mode 1777; 41 runs listed`, then

```
env.sh: sha256 3b38f198a5652ee3702d8694e130592ad5d277211ac53beba16140fa5bc3a05a, without blanks 46d5dff67fc5785d
count.py: sha256 7a3f1e11c77074e5c8a683ae7c4d85a07e2f1d04f0895d598f6adaed6fc721f8, without blanks a201f867df14f5c9
table.py: sha256 7496d2456729aedeb7e63e6b9c9c03a9feb3bcb9b755f6a8a50998501a5edf58, without blanks 51f8f439d2c83068
list.txt: sha256 3c734259f81d1d773ef5cbb7bbafe620311764c3b2b6cee8e32e05ffe4bee7c4, without blanks 318735ff77effc59
```

A file whose full hash differs but whose hash without blanks matches lost or
gained only spaces or tabs in the copy: that is fine for `env.sh` and
`list.txt`, and a stop for the two Python files (Python reads indentation). A
file whose hash without blanks differs was copied wrongly: stop.

Then check the trees as the build script left them, and point their results
at `$L/results`:

```sh
. /var/log/zts-kc-1026/env.sh
{
same "B head" "$(git -C $W/B rev-parse HEAD)" $HB
same "M head" "$(git -C $W/M rev-parse HEAD)" $HM
same "B's parent is M" "$(git -C $W/B rev-parse HEAD^)" $HM
same "B against M" "$(git -C $W/B diff --stat $HM HEAD | tail -1)" \
    " 5 files changed, 335 insertions(+), 10 deletions(-)"
same "B's files" "$(git -C $W/B diff --name-only $HM HEAD | xargs)" \
    "include/sys/zio_crypt.h module/os/freebsd/zfs/zio_crypt_os.c module/os/linux/zfs/zio_crypt_os_kcapi.c module/zfs/zio_crypt.c module/zfs/zio_crypt_os_icp.c"
same "files under tests/ changed by B" "$(git -C $W/B diff --name-only $HM HEAD -- tests | wc -l)" 0
for t in B M; do
	cd $W/$t
	same "$t tracked changes" "$(git diff HEAD --name-only | wc -l)" 0
	same "$t debug build (ZFS_DEBUG lines)" "$(grep -c '^#define ZFS_DEBUG 1$' zfs_config.h)" 3
	same "$t backend: ICP (no kcapi)" "$(grep -c '^#define HAVE_KERNEL_CRYPTO 1$' zfs_config.h)" 0
	same "$t build errors" "$(grep -cE ': error:|\*\*\* \[' $L/build-$t.log)" 0
	same "$t compiler warnings" "$(grep -cE ':[0-9]+:[0-9]+: warning:' $L/build-$t.log)" 0
	same "$t warnings that are not objtool's" \
	    "$(grep 'warning:' $L/build-$t.log | grep -vc 'warning: objtool:')" 0
	same "$t zed pid file" "$(grep -ao '/[[:alnum:]/._-]*/zed\.pid' .libs/zed | head -1)" $ZEDPID
	modinfo -F srcversion module/zfs.ko > $L/srcv-$t
	sha256sum module/zfs.ko module/spl.ko > $L/ko-$t.sha256
	sed -i "s|^BASEDIR = .*|BASEDIR = '$L/results'|" \
	    tests/test-runner/bin/test-runner.py.in tests/test-runner/bin/test-runner.py
	same "$t results directory" \
	    "$(grep -c "^BASEDIR = '$L/results'\$" tests/test-runner/bin/test-runner.py)" 1
	same "$t changes now" "$(git diff HEAD --name-only | xargs)" \
	    tests/test-runner/bin/test-runner.py.in
done
same "B srcversion" "$(cat $L/srcv-B)" $SRCV_B
same "M srcversion" "$(cat $L/srcv-M)" $SRCV_M
same "B and M srcversions differ" "$(sort -u $L/srcv-B $L/srcv-M | wc -l)" 2
same "B cache code: salt key hold, ICP session hook, 16 slots" \
    "$(grep -c 'zio_crypt_salt_key_hold(' $W/B/module/zfs/zio_crypt.c) $(grep -c '^zio_crypt_session_init_os(' $W/B/module/zfs/zio_crypt_os_icp.c) $(grep -cE 'define[[:space:]]+ZIO_CRYPT_SALT_CACHE_SIZE[[:space:]]+16$' $W/B/module/zfs/zio_crypt.c)" "2 1 1"
same "M has none of it" \
    "$(grep -c 'zio_crypt_salt_key_hold(' $W/M/module/zfs/zio_crypt.c) $(grep -c '^zio_crypt_session_init_os(' $W/M/module/zfs/zio_crypt_os_icp.c) $(grep -cE 'define[[:space:]]+ZIO_CRYPT_SALT_CACHE_SIZE[[:space:]]+16$' $W/M/module/zfs/zio_crypt.c)" "0 0 0"
} | tee $L/checks.txt
for t in B M; do
	echo "$t: objtool notes $(grep -c 'warning: objtool:' $L/build-$t.log)," \
	    "$(grep -E '^#define LIBFETCH_IS_(FETCH|LIBCURL) ' $W/$t/zfs_config.h | cut -d' ' -f2- | xargs)"
done | tee -a $L/checks.txt
```

Expected: 29 `ok:` lines and no `MISMATCH` (6, then 9 per tree, then 5), and
for each tree `objtool notes 83,` followed by the two `LIBFETCH_IS_FETCH` and
`LIBFETCH_IS_LIBCURL` values (recorded: if both are 0, libzfs has no https
fetcher and `zfs_load-key_https` fails on both trees). The `test-runner.py.in`
change is the results directory, and nothing else may differ. Any `MISMATCH`
means stop.

Then check the list against each tree's runfiles:

```sh
. /var/log/zts-kc-1026/env.sh
while read -r -u 3 label how arg n; do
	case $label in '#'*|'') continue ;; esac
	for t in B M; do
		if [ $how = -T ]; then
			python3 -I $L/count.py $W/$t $arg >> $L/list-sections-$t.txt
			got=$(tail -1 $L/list-sections-$t.txt | cut -d' ' -f2)
		else
			got=$(python3 -I $L/count.py $W/$t -t $arg | cut -d' ' -f2-)
		fi
		same "$t $label $arg" "$got" "$n"
	done
done 3< $L/list.txt > $L/list-check.txt
echo "list: $(grep -c '^ok:' $L/list-check.txt) ok, $(grep -c '^MISMATCH' $L/list-check.txt) mismatch" |
    tee -a $L/checks.txt
grep '^MISMATCH' $L/list-check.txt
cmp -s $L/list-sections-B.txt $L/list-sections-M.txt && echo "group sections: the same on B and M"
cat $L/list-sections-B.txt
```

Expected: `list: 82 ok, 0 mismatch`, `group sections: the same on B and M`,
then

```
  tests/functional/cli_root/zfs_change-key: 10 tests + 2 setup/cleanup
  tests/functional/cli_root/zfs_load-key: 7 tests + 2 setup/cleanup
  tests/functional/cli_root/zfs_unload-key: 3 tests + 2 setup/cleanup
zfs_load-key,zfs_unload-key,zfs_change-key 26
  tests/functional/send_xdr_encoding: 14 tests + 2 setup/cleanup
send_xdr_encoding 16
  tests/functional/cli_root/zfs_send: 13 tests + 2 setup/cleanup
zfs_send 15
  tests/functional/dedup: 16 tests + 0 setup/cleanup
  tests/functional/dedup:Linux: 1 tests + 2 setup/cleanup
dedup 19
```

A `MISSING` (a listed test absent or not executable), an `UNLISTED` (a `-t`
test that no runfile section lists) or any other count is a `MISMATCH`: stop.

## STEP 2: baselines

```sh
. /var/log/zts-kc-1026/env.sh
date '+%F %T' > $L/zts-start.txt
sudo -n losetup -a > $L/losetup-before.txt
sudo -n dmsetup ls > $L/dmsetup-before.txt 2>&1
ls -A / > $L/root-before.txt
sudo -n dmesg | kfilter | grep -ciE "$KRE" > $L/dmesg-baseline.txt
sudo -n journalctl -k -b --no-pager | kfilter > $L/journal-boot.txt
echo "baseline at $(cat $L/zts-start.txt): loop '$(xargs < $L/losetup-before.txt)'," \
    "dm '$(xargs < $L/dmsetup-before.txt)', dmesg $(cat $L/dmesg-baseline.txt) and journal" \
    "(this boot) $(grep -ciE "$KRE" $L/journal-boot.txt) suspicious lines," \
    "$(grep -c '^DEPOT SPLAT' $L/journal-boot.txt) stack depot splat(s); / holds" \
    "$(wc -l < $L/root-before.txt) entries; zpool.cache" \
    "$(stat -c '%s bytes' /etc/zfs/zpool.cache 2>/dev/null || echo absent)" | tee $L/baseline.txt
```

Expected: one `baseline at ...` line; the counts are recorded only (earlier
tasks left what they left), except `zpool.cache 0 bytes`.

## STEP 3: ZTS on B

```sh
. /var/log/zts-kc-1026/env.sh
if switch_to B none dbg && cd $W/B && run_list b; then r=done; else r=stopped; fi
dmesg_state B
echo "STEP 3 $r $(date '+%T')" | tee -a $L/steps.txt
```

Expected first: `load B rc=0`, `B guard ok: srcversion
89263B1C179E680CDDE56B0, zfs_flags 4294932990, coresize <n>,
spl_panic_halt=0` and `B: no pool after the load`. Then, for each of the 41
runs, in the order of `list.txt`:

```
b-kg -T zfs_load-key,zfs_unload-key,zfs_change-key PASS x26 (26 of 26 lines) rc=0 wall=<s>s
kernel b-kg: 0 suspicious lines, 0 stack depot splats
  zpool.cache: 0 bytes
leftovers after b-kg: none
b-r01 -t rsend/recv_dedup_encrypted_zvol PASS x3 (3 of 3 lines) rc=0 wall=<s>s
...
b-z1 -t cli_root/zdb/zdb_encrypted PASS x1 (1 of 1 lines) rc=0 wall=<s>s
...
b-dg -T dedup PASS x19 (19 of 19 lines) rc=0 wall=<s>s
```

each followed by its kernel, `zpool.cache` and leftovers lines; then `list
done on B`, the `dmesg B` line and `STEP 3 done <time>`. Every run's result
line is `PASS x<n>` with `n` the count in its label's row of the table above,
`rc=0`, `0 suspicious lines` and `leftovers: none`. Rough wall times: `kg`
1-3 min, each `r` and `c` run 0.5-2 min (`r15`
`send_realloc_encrypted_files` and the corrective `c08`-`c10` up to 4 min),
`l2` 1-3 min, `s2` 2-5 min, `dg` 3-6 min, the others under a minute.

A run that does not pass is reported by its own lines: the `Test:` line of
each line that did not pass, the `note:` lines (an `ERROR:`, a `cannot`, an
`unsupported`), the first failing line and the first `ERROR:` line. It is not
a stop by itself: the list goes on. What stops it: a kernel line matching a panic,
VERIFY, ASSERT, BUG, oops or hung task (`$L/PANIC`); an outer timeout; a
leftover (`$L/HELD` for a pool, device, mount or process); a run whose number
of `Test:` lines differs from its row (the runner did not run what the list
says); or a guard failure. A new entry in `/` or a non-fatal file in
`$W/files` is noted, not a stop.

## STEP 4: ZTS on M, and B's failures once more

Only if `$L/steps.txt` says `STEP 3 done`:

```sh
. /var/log/zts-kc-1026/env.sh
if grep -q '^STEP 3 done' $L/steps.txt && [ ! -e $L/PANIC ] && [ ! -e $L/HELD ] &&
    switch_to M B dbg && cd $W/M && run_list m && rerun_b_failures; then r=done; else r=stopped; fi
dmesg_state M
echo "STEP 4 $r $(date '+%T')" | tee -a $L/steps.txt
```

Expected: `unload B rc=0`, `load M rc=0`, `M guard ok: srcversion
8852710DB871A7587B9E722, zfs_flags 4294932990, coresize <n>, spl_panic_halt=0`,
`M: no pool after the load`, then the same 41 runs labelled `m-...` with the
same results as on B, `list done on M`, `reruns on M: 0` (or one `mr-...` run
per test that FAILED or was KILLED on B: a `-t` run again as a whole, a test
of a group alone, a group whose setup or cleanup failed as a group; at most
12), the `dmesg M` line and `STEP 4 done <time>`.

## STEP 5: B against M, and the kernel over the whole ZTS part

```sh
. /var/log/zts-kc-1026/env.sh
{
sudo -n journalctl -k --since "$(cat $L/zts-start.txt)" --no-pager -o short-iso 2>&1 |
    kfilter > $L/kernf-zts.txt
echo "kernel, whole ZTS part: $(grep -ciE "$KRE" $L/kernf-zts.txt) suspicious lines," \
    "$(grep -c '^DEPOT SPLAT' $L/kernf-zts.txt) stack depot splats"
grep -iE "$KRE" $L/kernf-zts.txt | head -40
for p in b m; do
	echo "$p: $(cat $L/$p-*.log 2>/dev/null | grep -E "$TRE" | tally)" \
	    "in $(grep -c "^$p-" $L/zts-results.txt) runs"
done
grep -h '^Missing util' $L/b-kg.log $L/m-kg.log 2>/dev/null | sort -u
compare_zts
ls $L/PANIC $L/HELD 2>&1
t=$(cat $L/loaded-tree 2>/dev/null)
if [ ! -e $L/PANIC ] && [ ! -e $L/HELD ] && [ -n "$t" ]; then
	(cd $W/$t && sudo -n ./scripts/zfs.sh -u) > $L/unload.log 2>&1
	echo "unload $t rc=$?"; rm -f $L/loaded-tree $L/loaded-srcv
fi
lsmod | grep -E '^(zfs|spl) '
echo "/var/tmp free: $(df -BG --output=avail /var/tmp | tail -1 | tr -d ' ')"
} 2>&1 | tee $L/compare.txt
```

Expected:

```
kernel, whole ZTS part: 0 suspicious lines, 0 stack depot splats
b: PASS x183 in 41 runs
m: PASS x183 in 41 runs
Missing util(s): <recorded only>
kg   zfs_load-key,zfs_unload-key,zfs_change-key   B: PASS x26                  M: PASS x26
r01  rsend/recv_dedup_encrypted_zvol              B: PASS x3                   M: PASS x3
...
dg   dedup                                        B: PASS x19                  M: PASS x19
ls: cannot access '/var/log/zts-kc-1026/PANIC': No such file or directory
ls: cannot access '/var/log/zts-kc-1026/HELD': No such file or directory
unload M rc=0
/var/tmp free: <n>G
```

with no `on both`, `B ONLY` or `M only` line and no `rerun on M` line. The
`kernel` and `unload` lines are checks; the rest is the result. If `$L/PANIC`
or `$L/HELD` exists, the module stays loaded: go to STEP 9.

## STEP 6: release trees B-rel and M-rel

Only if STEP 4 is done, nothing is held, no module is loaded, fio is present
and `/var/tmp` has at least 10G free:

```sh
. /var/log/zts-kc-1026/env.sh
echo "fio: ${FIO:-absent}; /var/tmp free: $(df -BG --output=avail /var/tmp | tail -1 | tr -d ' ')" |
    tee -a $L/perf.txt
r="not run"
if grep -q '^STEP 4 done' $L/steps.txt && [ ! -e $L/PANIC ] && [ ! -e $L/HELD ] && [ -n "$FIO" ] &&
    ! lsmod | grep -qE '^(zfs|spl) ' &&
    [ "$(df -BG --output=avail /var/tmp | tail -1 | tr -dc 0-9)" -ge 10 ]; then
	r=stopped
	for t in B-rel:$HB M-rel:$HM; do
		git -C $W/repo worktree add --detach $W/${t%%:*} ${t#*:} >> $L/worktrees-rel.log 2>&1
		echo "worktree ${t%%:*} rc=$? at $(git -C $W/${t%%:*} rev-parse --short=10 HEAD)"
	done | tee -a $L/perf.txt
	if build_rel B-rel B && build_rel M-rel M; then
		same "B-rel and M-rel srcversions differ" "$(sort -u $L/srcv-B-rel $L/srcv-M-rel | wc -l)" 2 |
		    tee -a $L/perf.txt
		grep -q '^MISMATCH: B-rel and M-rel' $L/perf.txt || r=done
	fi
fi
echo "/var/tmp free: $(df -BG --output=avail /var/tmp | tail -1 | tr -d ' ')" | tee -a $L/perf.txt
echo "STEP 6 $r $(date '+%T')" | tee -a $L/steps.txt
```

Expected: `fio: <path>; /var/tmp free: <n>G`, `worktree B-rel rc=0 at
3239e81a1d`, `worktree M-rel rc=0 at a128b8a075`, then for each tree `build
<tree> rc=0 wall=<s>s` (about as long as a debug build), 5 `ok:` lines (head,
no tracked change, `ZFS_DEBUG lines` 1, no kcapi, no build error),
`<tree> compiler warnings: 0, objtool notes: <n>` (recorded: a compiler
warning here is quoted from the log, not a stop), and `B-rel srcversion
89263B1C179E680CDDE56B0 (B: 89263B1C179E680CDDE56B0)`, `M-rel srcversion
8852710DB871A7587B9E722 (M: 8852710DB871A7587B9E722)` (equal to the debug
tree's, since srcversion hashes the sources only; a different value is
recorded, not a stop: the guard uses each tree's own), then `ok: B-rel and
M-rel srcversions differ`, the free space and `STEP 6 done <time>`. `STEP 6
not run` (fio absent, or an earlier stop) skips STEPS 7 and 8.

## STEP 7: the fio pool, written once

```sh
. /var/log/zts-kc-1026/env.sh
{
echo "fio $($FIO --version), mpstat ${MPSTAT:-absent}, bpftrace ${BPF:-absent}"
free -m | sed -n 2p
} | tee -a $L/perf.txt
if grep -q '^STEP 6 done' $L/steps.txt && switch_to M-rel none rel && perf_prep; then
	r=done
else
	r=stopped
fi
echo "STEP 7 $r $(date '+%T')" | tee -a $L/steps.txt
```

Expected: the fio version and the two tool paths or `absent`, the memory line,
then `load M-rel rc=0`, `M-rel guard ok: srcversion 8852710DB871A7587B9E722,
zfs_flags 0, coresize <n>, spl_panic_halt=0`, `M-rel: no pool after the load`,
the 16 property lines (`encryption aes-256-gcm`, `keyformat raw`,
`encryptionroot` the dataset itself, `recordsize 16K` and `4K`,
`primarycache metadata`, `compression off`, `atime off`, `checksum on`), `write
r16k: 2048 MiB, <n> KiB/s, error 0, wall <s>s`, `write r4k: 1024 MiB, ...`,
`keys unloaded and loaded again: every block is now under an older salt`,
`reference hashes: ...`, `space: kcperf/r16k <bytes> kcperf/r4k <bytes>, vdev
file <n> MiB on disk` (near 2.2e9 and 1.1e9 bytes, and about 3200 MiB), `ok: prep down:
exported, no pool, mount or process left` and `STEP 7 done`. About 5 minutes.

## STEP 8: five rounds, M-rel then B-rel

```sh
. /var/log/zts-kc-1026/env.sh
if grep -q '^STEP 7 done' $L/steps.txt && perf_rounds; then r=done; else r=stopped; fi
echo "STEP 8 $r $(date '+%T')" | tee -a $L/steps.txt
python3 -I $L/table.py $L/perf/results.tsv $(nproc) $RT | tee $L/perf-table.txt
```

Expected, for each of the 10 loads (round 1 M-rel, round 1 B-rel, ...,
round 5 B-rel):

```
== round <r> <tree> <time> load <loadavg>
unload <previous> rc=0
load <tree> rc=0
<tree> guard ok: srcversion <its srcversion>, zfs_flags 0, coresize <n>, spl_panic_halt=0
<tree>: no pool after the load
ok: <r>-<tree> up: pool, keys, mounts, files, parameters
<r>-<tree>-16k IOPS <n> reads <n> lat <x>us sys <x>% usr <x>% steal <x>% idle <x>% mpstat-sys <x>% pgpgin <n>KiB warm-reads <n> hkdf <n>
<r>-<tree>-4k IOPS ...
ok: <r>-<tree> down: exported, no pool, mount or process left
kernel perf-<r>-<tree>: 0 suspicious lines, 0 stack depot splats
```

with, in round 5 after B-rel's two phases, `ok: 5-B-rel r16k read back by
B-rel as M-rel read it` and the same for `r4k`; then `perf rounds done`,
`STEP 8 done` and the table:

```
rs   tree   n  IOPS                     %sys                 CPU us/read          %steal  pgpgin    hkdf/read (warm-up)
16k  M-rel  5  <median> [<min>-<max>]   ...
16k  B-rel  5  ...
16k  B/M    IOPS <+x>%, CPU per read <-x>%, %sys <+-x> points (medians)
4k   M-rel  5  ...
4k   B-rel  5  ...
4k   B/M    ...
per round (IOPS M-rel/B-rel): ...
hkdf calls in each warm-up (M-rel, B-rel): ...
```

Checks: every `ok:` line as shown, the guard lines, `0 suspicious lines`, both
hashes equal, `fio` rc 0. Recorded only: the numbers. `pgpgin` should be near
0 KiB (reads served from the page cache); `%steal` above a few percent means
the hypervisor was busy then. `hkdf` reads `n/a` without bpftrace. About 2
minutes per load, 20 to 25 minutes in all.

## STEP 9: final checks, cleanup and report

```sh
. /var/log/zts-kc-1026/env.sh
{
sudo -n journalctl -k --since "$(cat $L/zts-start.txt)" --no-pager -o short-iso 2>&1 |
    kfilter > $L/kernf-all.txt
echo "kernel, whole task: $(grep -ciE "$KRE" $L/kernf-all.txt) suspicious lines," \
    "$(grep -c '^DEPOT SPLAT' $L/kernf-all.txt) stack depot splats"
grep -iE "$KRE" $L/kernf-all.txt | head -40
ls $L/PANIC $L/HELD 2>&1
cat $L/steps.txt
t=$(cat $L/loaded-tree 2>/dev/null)
if [ -n "$t" ] && [ "$(cat /sys/module/zfs/srcversion 2>/dev/null)" = "$(cat $L/srcv-$t)" ] &&
    [ ! -e $L/PANIC ] && [ ! -e $L/HELD ]; then
	cd $W/$t
	[ "$(sudo -n ./zpool list -H -o name 2>&1)" = kcperf ] && sudo -n ./zpool export kcperf &&
	    echo "kcperf exported"
	cd $W && (cd $W/$t && sudo -n ./scripts/zfs.sh -u) > $L/unload.log 2>&1
	echo "unload $t rc=$?"
else
	echo "no unload: loaded tree '${t:-none}', PANIC/HELD: $(ls $L/PANIC $L/HELD 2>/dev/null | xargs)"
fi
lsmod | grep -E '^(zfs|spl) '
pgrep -ax 'zed|lt-zed'
others 'fio |bpftrace|mpstat|test-runner\.py|zfs-tests\.sh'
ls -l /etc/zfs/vdev_id.conf $ZEDPID /tmp/ZTS-snakeoil.log 2>&1
echo "zpool.cache: $(stat -c '%s bytes' /etc/zfs/zpool.cache 2>/dev/null || echo absent)"
ls -A / | diff $L/root-before.txt - && echo "/ as at the start"
echo "/var/tmp free: $(df -BG --output=avail /var/tmp | tail -1 | tr -d ' ')"
} 2>&1 | tee $L/cleanup.txt
```

Expected: `kernel, whole task: 0 suspicious lines, 0 stack depot splats`, the
two `No such file or directory` lines for `PANIC` and `HELD`, the step lines
(`STEP 3 done` to `STEP 8 done`), `unload B-rel rc=0` (or `no unload: loaded
tree 'none', PANIC/HELD: ` when the fio part did not run and STEP 5 already
unloaded M), then nothing for the
module, `zed` and process lines, `No such file or directory` for the three
files, `zpool.cache: 0 bytes`, `/ as at the start` and the free space. When
something is held or a panic was seen, the module stays loaded and `$W` stays:
report it, the maintainer decides.

Write the report before anything is removed:

```sh
. /var/log/zts-kc-1026/env.sh
{
echo "VM 105, kernel ZTS and fio A/B of tigerblue77/zfs:zio-crypt-salt-key-cache"
echo "3239e81a1d against its parent a128b8a075 (VM-TESTS-105-kc.md)"
for s in step0:"STEP 0" files:"STEP 1 files" checks:"STEP 1 checks" baseline:"STEP 2" \
    steps:"steps" runs:"runs (ZTS and module loads)" compare:"STEP 5: B against M" \
    perf:"fio part" perf-table:"fio table" cleanup:"STEP 9"; do
	echo; echo "== ${s#*:}"
	cat $L/${s%%:*}.txt 2>&1
done
echo; echo "== per-phase fio rows (label iops sys usr steal mpstat-sys pgpgin warm-reads hkdf reads)"
cat $L/perf/results.tsv 2>&1
} > $L/REPORT.txt
wc -l $L/REPORT.txt
```

Then add, by hand, at the end of `REPORT.txt`: a `== summary` section that
names the case of "What the results mean" below that applies to the ZTS part
and to the fio part, with the figures it rests on, and a `== deviations` section with
anything that differed from these steps and why (`none` if nothing did). Only
then, if neither `$L/PANIC` nor `$L/HELD` exists and no module is loaded:

```sh
. /var/log/zts-kc-1026/env.sh
if [ ! -e $L/PANIC ] && [ ! -e $L/HELD ] && ! lsmod | grep -qE '^(zfs|spl) '; then
	rm -rf /var/tmp/zts-kc-1026
fi
{
echo; echo "== removal"
ls -d /var/tmp/zts-kc-1026 2>&1
echo "/var/tmp free: $(df -BG --output=avail /var/tmp | tail -1 | tr -d ' ')"
echo "logs kept in $L: $(ls $L | wc -l) entries"
} | tee -a $L/REPORT.txt
```

Expected: `ls: cannot access '/var/tmp/zts-kc-1026': No such file or
directory`, about 20G free again, and the log count. Keep `$L`. Then reply
with the whole of `REPORT.txt`.

## What the results mean

ZTS part:

- **As expected**: every run passes on B and on M, no kernel line, no
  leftover. In the kernel, on the ICP backend and under real concurrency, the
  cache passes every test that reaches it here: reads after key reloads,
  imports and legacy pool images, raw sends and receives, encrypted dedup
  data read back whole, L2ARC and corrective-receive re-encryption, thorough
  scrubs, block cloning and ZIL replay on encrypted datasets.
- **A test fails on B only**: the branch's until shown otherwise. Its lines,
  its `m-` run that passed and its `mr-` rerun on M are the result; a failure
  in `kg`, `r`, `c`, `i`, `k` or `s` runs points at a wrong key for an older
  salt (a checksum or MAC error, `EIO`, a received stream that differs), a
  failure in `z1` or `z2` at the userspace build.
- **A test fails on both trees**: master's or this VM's (fio, openssl,
  `xxh128sum`, the https fetcher, `compatibility.d`), not the change's.
- **A test fails on M only**: reported; it says nothing against the branch.
- **A run is not the same twice** (an `m-` run and its `mr-` rerun differ):
  reported run by run, as a flaky test.
- **Any kernel VERIFY, ASSERT, panic, oops or hung task**: a different
  problem, and the most important result: the line, the run and the tree.
  A `zio_crypt_salt_*` function or `zsc_lock` in it is the change's.

fio part:

- **`hkdf/read`**: about 1.00 on M-rel (every block read derives a key: the
  slow path), and close to 0 on B-rel (a handful of calls per warm-up: the
  first two misses of the one salt, plus a few concurrent first misses).
  That is VM-NOTE's "the slow path is gone".
- **IOPS and CPU per read**: B-rel at least M-rel at both sizes, the gain
  larger at 4k than at 16k (in userspace the decryption alone was 63-65%
  faster at 4K and 33-37% at 16K; the rest of the read path dilutes that
  here). Medians within about 3% with overlapping ranges mean no measurable
  difference on this VM. Remember the `slub_debug` bias above.
- **`pgpgin` far above 0**: those reads went to the disk, so that cell is
  bound by I/O and not by the CPU: say so next to it.
- **A hash mismatch**: B-rel read back data that differs from what M-rel
  read: a correctness failure of the branch, whatever the speed.

## REPORT.txt expected

`<...>` is what the run fills in; everything else is what these instructions
expect.

```
VM 105, kernel ZTS and fio A/B of tigerblue77/zfs:zio-crypt-salt-key-cache
3239e81a1d against its parent a128b8a075 (VM-TESTS-105-kc.md)

== STEP 0
tigerblue77 / <6.12 kernel> / Debian GNU/Linux 13 / 16 / 0002 / <CPU> / cmdline: slub_debug=FZPU /
<memory> / /var/tmp free: <n>G / no module, process or zed / zpool.cache 0 bytes /
stack depot warnings this boot: 1 / tools: <paths or absent> / W holds: B M repo
builds finished (check 1): B 89263B1C179E680CDDE56B0, M 8852710DB871A7587B9E722

== STEP 1 files
<the four sha256 lines, as listed in STEP 1>

== STEP 1 checks
29 ok lines; B: objtool notes 83, <LIBFETCH values>; M: objtool notes 83, <LIBFETCH values>
list: 82 ok, 0 mismatch

== STEP 2
baseline at <date>: loop '<...>', dm '<...>', dmesg <n> and journal (this boot) <n> suspicious lines,
<n> stack depot splat(s); / holds <n> entries; zpool.cache 0 bytes

== steps
STEP 3 done <time>
STEP 4 done <time>
STEP 6 done <time>
STEP 7 done <time>
STEP 8 done <time>

== runs (ZTS and module loads)
load B rc=0
B guard ok: srcversion 89263B1C179E680CDDE56B0, zfs_flags 4294932990, coresize <n>, spl_panic_halt=0
B: no pool after the load
b-kg -T zfs_load-key,zfs_unload-key,zfs_change-key PASS x26 (26 of 26 lines) rc=0 wall=<s>s
kernel b-kg: 0 suspicious lines, 0 stack depot splats
  zpool.cache: 0 bytes
leftovers after b-kg: none
<the same four lines for b-r01 to b-dg: PASS x3 for each r, c, l2, s, i and k run, PASS x1 for z1 and z2,
 PASS x16 for xg, PASS x15 for sg, PASS x19 for dg>
list done on B
dmesg B: <n> suspicious lines in the buffer (baseline <n>; a failed test clears it)
unload B rc=0
load M rc=0
M guard ok: srcversion 8852710DB871A7587B9E722, zfs_flags 4294932990, coresize <n>, spl_panic_halt=0
M: no pool after the load
<m-kg to m-dg: the same results as on B>
list done on M
reruns on M: 0
dmesg M: <n> suspicious lines in the buffer (baseline <n>; a failed test clears it)
load M-rel rc=0
M-rel guard ok: srcversion 8852710DB871A7587B9E722, zfs_flags 0, coresize <n>, spl_panic_halt=0
M-rel: no pool after the load
<for each round: unload, load, guard and no-pool lines, and kernel perf-<r>-<tree>: 0 suspicious lines, 0 stack depot splats>

== STEP 5: B against M
kernel, whole ZTS part: 0 suspicious lines, 0 stack depot splats
b: PASS x183 in 41 runs
m: PASS x183 in 41 runs
Missing util(s): <...>
<41 lines, label, run, B: PASS x<n>, M: PASS x<n>; no on both, B ONLY or M only line>
unload M rc=0
/var/tmp free: <n>G

== fio part
fio: <path>; /var/tmp free: <n>G
worktree B-rel rc=0 at 3239e81a1d
worktree M-rel rc=0 at a128b8a075
build B-rel rc=0 wall=<s>s
<5 ok lines>
B-rel compiler warnings: 0, objtool notes: <n>
B-rel srcversion 89263B1C179E680CDDE56B0 (B: 89263B1C179E680CDDE56B0)
build M-rel rc=0 wall=<s>s
<5 ok lines>
M-rel compiler warnings: 0, objtool notes: <n>
M-rel srcversion 8852710DB871A7587B9E722 (M: 8852710DB871A7587B9E722)
ok: B-rel and M-rel srcversions differ
/var/tmp free: <n>G
fio fio-<version>, mpstat <path or absent>, bpftrace <path or absent>
Mem: <...>
<16 property lines>
write r16k: 2048 MiB, <n> KiB/s, error 0, wall <s>s
write r4k: 1024 MiB, <n> KiB/s, error 0, wall <s>s
keys unloaded and loaded again: every block is now under an older salt
reference hashes: r16k <16 hex>..., r4k <16 hex>..., read in <s>s
space: kcperf/r16k <bytes> kcperf/r4k <bytes>, vdev file <n> MiB on disk
ok: prep down: exported, no pool, mount or process left
== round 1 M-rel <time> load <...>
ok: 1-M-rel up: pool, keys, mounts, files, parameters
1-M-rel-16k IOPS <n> reads <n> lat <x>us sys <x>% usr <x>% steal <x>% idle <x>% mpstat-sys <x>% pgpgin <n>KiB warm-reads <n> hkdf <n>
1-M-rel-4k IOPS <...>
ok: 1-M-rel down: exported, no pool, mount or process left
<the same for 1-B-rel, 2-M-rel, ..., 5-B-rel, with in 5-B-rel:>
ok: 5-B-rel r16k read back by B-rel as M-rel read it
ok: 5-B-rel r4k read back by B-rel as M-rel read it
perf rounds done

== fio table
<the table of STEP 8>

== STEP 9
kernel, whole task: 0 suspicious lines, 0 stack depot splats
ls: cannot access '/var/log/zts-kc-1026/PANIC': No such file or directory
ls: cannot access '/var/log/zts-kc-1026/HELD': No such file or directory
<the step lines>
unload B-rel rc=0
ls: cannot access '/etc/zfs/vdev_id.conf': No such file or directory
ls: cannot access '/usr/local/var/run/zed.pid': No such file or directory
ls: cannot access '/tmp/ZTS-snakeoil.log': No such file or directory
zpool.cache: 0 bytes
/ as at the start
/var/tmp free: <n>G

== per-phase fio rows (label iops sys usr steal mpstat-sys pgpgin warm-reads hkdf reads)
<20 rows>

== summary
ZTS: as expected (183/183 PASS on B and on M, no kernel line, no leftover)
fio: hkdf/read <M> on M-rel, <B> on B-rel; IOPS B/M <+x>% at 16k, <+x>% at 4k; CPU per read <-x>%, <-x>%;
     pgpgin <n>, steal <n>; hashes equal

== deviations
none

== removal
ls: cannot access '/var/tmp/zts-kc-1026': No such file or directory
/var/tmp free: <n>G
logs kept in /var/log/zts-kc-1026: <n> entries
```

## Estimated duration

About 2 h 15 min to 3 h: STEPS 0-2 about 5 min, STEP 3 30-45 min, STEP 4
30-45 min (plus 1-5 min per rerun), STEP 5 2 min, STEP 6 20-30 min (two
release builds at `-j8`), STEP 7 about 5 min, STEP 8 20-25 min, STEP 9 about
5 min.
