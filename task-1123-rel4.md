TASK-1123-REL4 (from the coordinator session "main"): functional, on **VM 105 or VM 106** (Debian 13, Linux 6.12, 8 CPUs, 7.8 GB), about 1 h with its four builds, at most 1 h 45, not next to a performance run. **On VM 105 (CHSONVTST02) it must be over by 20:50Z:** there, do not start it after 19:05Z. No reboot is expected and none is approved. It gives four pull requests to openzfs/zfs, not opened yet, the release-build run their texts lack: their VM evidence comes from debug builds only. Four fork commits, each built once with a plain `./configure` (no `--enable-debug`), and on each module the runs that the earlier task made with the fix: no fail-before arm, no revert, no reboot. Nothing is published on GitHub, and no credential files are touched. Report: `/var/log/task-1123-rel4/report.md`, with the same text as your final message.

**Your messages to main are blocked:** every note for main is the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it).

The transport may show greater-than, less-than and ampersand as HTML entities: read them as the characters. The expected files of step 1 and the kit of step 2 contain all three. Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`D`, `L`, `F`, `B`, `P`, `t`, `r`, `T`).

## Plan and durations

| step | what | about |
|---|---|---|
| 0 | state of this VM | 3 min |
| 1 | four checkouts and their checks | 5 min |
| 2 | the runner kit | 2 min |
| 3 | four release builds, two at a time | 20 to 25 min |
| 4 | the runs, one tree at a time: `pnr`, `rt1`, `rt3`, `zpm` | 15 to 20 min |
| 5 | end and cleanup | 5 min |

## Why

| tree | pull request text | what the commits change | earlier VM run (debug build) | this task (release build) |
|---|---|---|---|---|
| `pnr` | 04-d, "zpool: reserve anymirror and anyraid for new pools only" | moves `anymirror` and `anyraid` from `pool_namecheck()` (kernel and libzfs, every pool ioctl) to `zpool_name_valid()` (userland, create and import only); new test `pool_names_reserved_anyraid` | TASK-1117-PNR, VM 106, tree A as fetched (FA): the new test 3/3 (00:01 to 00:02), `-T pool_names` 3/3, `zpool_create_007_neg` and `zpool_create_anyraid_005_neg` PASS | the same six runs. Not run: UA (fail-before) and tree B (#19774's fgroup names, another pull request) |
| `rt1` | 10-f, "ZTS: regression tests for the zdb leak status and autotrim=off fixes" | tests only: `zhack_metaslab_leak` also checks `zdb -b`'s exit status (#19606); new `autotrim_off_exit` (#19757) | TASK-1117-RT1, VM 105, branch `zts-regression-tests-1` at `be9f1a808f8` (now gone from the fork; same tree as this head): `autotrim_off_exit` 3/3 (00:02; K = 0, 2, 0), `zhack_metaslab_leak` 3/3 (00:11; 107 to 122 leaked segments, about 128 MiB leaked) | the same six runs. Not run: steps 3 and 4 (the fixes reverted) |
| `rt3` | 11-g, "ZTS: regression tests for the initialize/TRIM detach and gang header ABD fixes" | tests only: new `zpool_initialize_detach_load` and `zpool_trim_detach_load` (#19400), new `gang_blocks_header_read_fail` (#19391) | TASK-1117-RT3, VM 105, branch `zts-regression-tests-3` at `cd94d333eb1` (same tree as this head; only the commit messages differ): the two detach tests 3/3 each (00:17 to 00:18; space map 5520 to 6360 bytes; `loading_time` 15301 to 15470 ms), the gang test 3/3 (00:07; `struct_size` +448 bytes each) | the same nine runs. Not run: steps 3 to 5 (build U, the reverted runs, both reboots) |
| `zpm` | 24, "zinject: name -p in the panic option conflict message" | `cmd/zinject/zinject.c`: the panic branch's refusal names `panic (-p)` instead of `import\|export delay (-P)`; `zinject_args` checks it | none. The text's check ran outside ZTS, on the zinject binaries with a preload library in place of `/dev/zfs`; its body says "Not tested: `zinject_args` in a kernel outside CI" | `zinject_args` 3 times, and the refusals the text lists, run against the real `/dev/zfs` |

**Heads.** Each text names a head rebased onto upstream master `a1daebfec3f` that exists only in main's own repository. The fork holds the heads before that rebase, on upstream master `c913da86156`: this task fetches those. Main compared each pair with `git range-diff c913da86156..<fork head> a1daebfec3f..<text head>`:

```
pool-name-reserved-userland, 8ed89d77624 and the text's 33802c19a9f:
1:  8ed89d77624 = 1:  33802c19a9f zpool: reserve anymirror and anyraid for new pools only
zts-regression-tests-2, c414d739ed4 and the text's c6fabc72eb6:
1:  cc37d747df9 = 1:  b7a9e042688 ZTS: check zdb's exit status in zhack_metaslab_leak
2:  c414d739ed4 = 2:  c6fabc72eb6 ZTS: autotrim=off must stop the autotrim threads
zts-regression-tests-3-v2, ee3353054de and the text's 54fbc26b6f6:
1:  a96b9e178f0 = 1:  a36ef0b3c47 ZTS: detach a leaf while initialize or TRIM loads a metaslab
2:  ee3353054de = 2:  54fbc26b6f6 ZTS: check that a failed gang header read frees its ABD
zinject-panic-message, 4a33fbf4fce and the text's f59aeaecdb4:
1:  4a33fbf4fce = 1:  f59aeaecdb4 zinject: name -p in the panic option conflict message
```

Every commit is `=`: the same patch and the same message; only the base differs. The six master commits between `c913da86156` and `a1daebfec3f` (CI scripts; "FreeBSD: Fix append-only semantics", which also changes `module/zfs/zfs_vnops.c`; two ZTS changes) touch none of the files these commits change, apart from other lines of `tests/runfiles/common.run` and `tests/zfs-tests/tests/Makefile.am`. The texts will report these runs as runs of the same patches before their rebase onto `a1daebfec3f`.

**Release.** A plain `./configure` drops `ASSERT` (`VERIFY` stays) and builds userland with `-DNDEBUG` and without `-Werror`. None of these tests goes through an assertion:
- `pnr`: `pool_namecheck()` and `zpool_name_valid()` refuse a name with an error.
- `rt1`: the first test reads zdb's exit status (userland); the second counts the autotrim threads, which `spa.c` stops without an assertion.
- `rt3`: the detach bug was a NULL dereference, and the gang one a leak that `abdstats` counts in every build. The `metaslab_load:` line that the detach tests read is a `zfs_dbgmsg()`, which a release module keeps (only `dprintf()` is debug-only). Without `ZFS_DEBUG` an `abd_t` is smaller (its refcount is a bare counter); the threshold, 16 bytes per failed read, does not depend on it.
- `zpm`: one string in zinject's option checks, which run before any ioctl.

So each run is expected to give its debug result. A srcversion is computed from the module's sources, not from the build flags: `pnr`'s is expected to be `A1807D494E76F86869B2426` (PNR's FA), and the three others `65D8C0C24A9B688639F0721` (master `c913da86156`'s module, which RT1 and RT3 saw: their commits change tests and zinject only, so these three modules are built from the same sources). A different value is recorded, not a stop. Which build a tree holds is told by its `config.log` and its configure output; which module is loaded, by the load line ("(DEBUG mode)" or not) and `zfs_flags` (0 on a release module). Do not count `ZFS_DEBUG` in `zfs_config.h`: every `zfs_config.h` defines it in its in-kernel block (TASK-1123-CCM3).

## What is tested (fork https://github.com/tigerblue77/zfs)

Each head is fetched by its full SHA, into its own directory, and built once:

| tree | branch | head | tree hash | base |
|---|---|---|---|---|
| pnr | pool-name-reserved-userland | 8ed89d776244952394eb7906a15b6c103796754c | 5483f1c50efb72131bd8fccf86b6c6157bfe1249 | `HEAD~1` = upstream master `c913da86156eba3243cc1ef515e98d69887654a7` |
| rt1 | zts-regression-tests-2 | c414d739ed45716106fdbb4665a91ec3ac03d855 | 59b0dd65c2c0ef6c0da9771c1ee5835f8f39e832 | `HEAD~2` = the same master |
| rt3 | zts-regression-tests-3-v2 | ee3353054de510e348853e5ad6d869cf0fe3635a | 9d5bc40675e8c38d2d957a86e58ba118f7a89267 | `HEAD~2` = the same master |
| zpm | zinject-panic-message | 4a33fbf4fce5b136ce4a613205bd29fd8188b53f | 39b8d66f42002bef1659badb9a37dde80e6116e9 | `HEAD~1` = the same master |

`rt1` is named after TASK-1117-RT1, whose tests it reruns, although its branch is now `zts-regression-tests-2`. `zpm` stands for zinject panic message.

## VM rules

- Nothing is published on GitHub; fetching is fine. Never display, copy or create a credential file. Install no package.
- ZTS uses its own file vdevs and loop devices. Touch no block device of the VM. Never run `zpool import` yourself, `zpool import -a` or `zfs-tests.sh -x`.
- **One module and one ZTS run at a time.** The builds run two at a time, before any run (step 3).
- **Pre-run checks**, before every ZTS run, `<t>` being the tree whose module is loaded. `runs.sh` (step 2) makes them itself before each of its runs and writes a `STOP` line if one fails:
  - `cat /sys/module/zfs/srcversion` equals `cat $L/sv-<t>`;
  - `timeout 30 sudo -n $D/<t>/zpool list -H -o name` shows no pool;
  - the first line of `timeout 30 sudo -n $D/<t>/zinject` is `No handlers registered.`;
  - `sudo -n losetup -a` shows nothing;
  - none of `/var/tmp/file-vdev*`, `/var/tmp/tunable-*`, `/var/tmp/testdir`, `/var/tmp/pool_names_reserved`, `/var/tmp/trim-vdev*`, `/var/tmp/zpool_initialize_detach_load.*`, `/var/tmp/zpool_trim_detach_load.*` exists;
  - `/etc/zfs/zpool.cache` is absent or empty (the module truncates it when the last pool goes).
- **Every build and every series of runs runs detached** (`setsid nohup ... &`) and is polled.
- **Hangs.** A pair of builds still going after 45 minutes, or a series whose `$L/runs.txt` has not changed for 20 minutes (one run takes a minute at most), is a hang. Capture into `$L/hang.txt`: `ps -eLo pid,tid,stat,etime,comm,wchan:32 | grep -E ' D|zfs-tests|test-runner|ksh|zpool|zfs|zdb|zhack|zinject|make|cc1'`; `/proc/<tid>/stack` of each thread in state D, read in the background (`(sudo -n cat <file> > <out> 2>&1 &); sleep 3; cat <out>`); `tail -n 200 /proc/spl/kstat/zfs/dbgmsg`; `sudo -n dmesg | tail -n 150`. Then stop and report. **Do not reboot.**
- **Kernel log.** A follower keeps it from step 4 on (a failing test's callback runs `dmesg -c`, so `dmesg` alone can lose lines). After each tree's runs, check its lines since the tree's mark for `VERIFY|ASSERT|PANIC|BUG:|Oops|general protection|Call Trace|blocked for more than|possible circular|deadlock|KFENCE|KASAN|segfault|WARNING`. None is expected anywhere in this task. Attribute any line to the run of the time, from the `task-1123-rel4: <tag>-<n> start` marks and the `ZTS run ...` lines that `-K` writes. Any line but a `WARNING` one is a finding: quote it with the 10 lines before it, capture as for a hang, and stop. A `WARNING` line with no `Call Trace` after it (a pool suspended and resumed, say) is quoted and counted, and the task goes on.
- **A run that does not pass** does not stop the task, apart from the two cases of step 4c: quote its `Test:` line and, from its own output file (step 4), `grep -n -B 20 -m 1 -E 'ERROR|FAIL'`; say whether `tests/test-runner/bin/zts-report.py.in` lists it as known (none of these tests is listed on master `c913da86156`); then go on without rerunning it. `runs.sh` stops the series by itself if the run left a pool, a handler, a loop device or a file behind: a `STOP` line is a stop.
- A module that will not unload: capture as for a hang, stop and report.
- **On VM 105:** no build pair and no series starts after 20:30Z. If that time comes first, let what runs finish, go to step 5, and report what ran.
- Run `sudo -n fstrim -av` after deleting the trees.

## Step 0: state

Check and quote:
- `date -u`, `hostname` (CHSONVTST02 is VM 105, CHSONVTST03 is VM 106), `uname -r`, `uptime -s`, `nproc`, `free -m`. On VM 105, a time after 19:05Z is a stop: report it and change nothing;
- `df -h / /var/tmp`: at least 25 GiB free on `/var/tmp` (four built trees), else stop and report;
- `lsmod | grep -E '^(zfs|spl) '`: must be empty. If a pool is imported, stop and report;
- `ls -l /dev/zfs 2>&1`: must not exist;
- `id zts`, `command -v ksh git python3 lsblk truncate` (`python3` writes the labels of `pnr`'s test; `lsblk` is how the trim groups' setups find discard support), `sudo -n true && echo sudo-ok`;
- `pgrep -a -x zed; pgrep -a -x lt-zed`: must be empty (a running zed is a stop: do not start or stop one);
- `pgrep -af 'dmesg --follow'`: quote it (one left by an earlier task stays as it is);
- `sudo -n losetup -a`: empty;
- `ls -d /var/tmp/task-1123-rel4 /var/log/task-1123-rel4 /var/tmp/file-vdev* /var/tmp/tunable-* /var/tmp/testdir /var/tmp/pool_names_reserved /var/tmp/trim-vdev* /var/tmp/zpool_initialize_detach_load.* /var/tmp/zpool_trim_detach_load.* 2>&1`: none exists;
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or size 0. Above 0 is a stop: quote its `ls -l`, do not read it;
- `sysctl kernel.panic_on_oops kernel.hung_task_panic`: quote them; change nothing;
- `ps -eo pid=,comm= | grep -wE 'fio|make|cc1|ztest|zfs-tests.sh|zloop.sh|configure'`: nothing (a build or a test of another task is a stop).

## Step 1: fetch and check

```sh
D=/var/tmp/task-1123-rel4; L=/var/log/task-1123-rel4; F=https://github.com/tigerblue77/zfs
sudo -n mkdir -p $D $L && sudo -n chown "$(id -un):" $D $L && chmod 755 $D && ls -A /var/tmp > $L/vartmp-0.txt
echo "$(date -u +%FT%TZ) start $(hostname) $(uname -r)" >> $L/progress.md
for t in "pnr 8ed89d776244952394eb7906a15b6c103796754c" "rt1 c414d739ed45716106fdbb4665a91ec3ac03d855" "rt3 ee3353054de510e348853e5ad6d869cf0fe3635a" "zpm 4a33fbf4fce5b136ce4a613205bd29fd8188b53f"; do set -- $t
  mkdir $D/$1 && cd $D/$1 && git init -q . && git fetch -q --depth 3 $F $2 && git checkout -q FETCH_HEAD && echo "FETCH-OK $1"
done
```

Expected `FETCH-OK pnr`, `FETCH-OK rt1`, `FETCH-OK rt3`, `FETCH-OK zpm`. If a fetch fails, wait 1 minute and run that tree's `git fetch -q --depth 3 $F <head> && git checkout -q FETCH_HEAD` once more, in its directory. If it fails again, fetch that tree's branch by name instead, in the same directory (`git fetch -q --depth 3 $F <branch> && git checkout -q FETCH_HEAD`), say so, and go on: the checks below stop on a branch that has moved.

```sh
D=/var/tmp/task-1123-rel4; L=/var/log/task-1123-rel4; B=c913da86156eba3243cc1ef515e98d69887654a7
for t in "pnr 1" "rt1 2" "rt3 2" "zpm 1"; do set -- $t; cd $D/$1 &&
  echo "$1 $(git rev-parse HEAD) $(git rev-parse 'HEAD^{tree}') $(git rev-parse HEAD~$2) $(git status --porcelain | wc -l)" &&
  git log --format="$1 %H %an | %s" $B..HEAD && echo "$1:$(git diff --shortstat $B HEAD)"
done > $L/trees.txt
cat > $L/trees.expected <<'EOF'
pnr 8ed89d776244952394eb7906a15b6c103796754c 5483f1c50efb72131bd8fccf86b6c6157bfe1249 c913da86156eba3243cc1ef515e98d69887654a7 0
pnr 8ed89d776244952394eb7906a15b6c103796754c Tigerblue77 | zpool: reserve anymirror and anyraid for new pools only
pnr: 7 files changed, 146 insertions(+), 8 deletions(-)
rt1 c414d739ed45716106fdbb4665a91ec3ac03d855 59b0dd65c2c0ef6c0da9771c1ee5835f8f39e832 c913da86156eba3243cc1ef515e98d69887654a7 0
rt1 c414d739ed45716106fdbb4665a91ec3ac03d855 Tigerblue77 | ZTS: autotrim=off must stop the autotrim threads
rt1 cc37d747df931bea3f512d52edd0e715073feb2a Tigerblue77 | ZTS: check zdb's exit status in zhack_metaslab_leak
rt1: 4 files changed, 115 insertions(+)
rt3 ee3353054de510e348853e5ad6d869cf0fe3635a 9d5bc40675e8c38d2d957a86e58ba118f7a89267 c913da86156eba3243cc1ef515e98d69887654a7 0
rt3 ee3353054de510e348853e5ad6d869cf0fe3635a Tigerblue77 | ZTS: check that a failed gang header read frees its ABD
rt3 a96b9e178f0b2028273fce522c019932eeaf1029 Tigerblue77 | ZTS: detach a leaf while initialize or TRIM loads a metaslab
rt3: 6 files changed, 385 insertions(+), 2 deletions(-)
zpm 4a33fbf4fce5b136ce4a613205bd29fd8188b53f 39b8d66f42002bef1659badb9a37dde80e6116e9 c913da86156eba3243cc1ef515e98d69887654a7 0
zpm 4a33fbf4fce5b136ce4a613205bd29fd8188b53f Tigerblue77 | zinject: name -p in the panic option conflict message
zpm: 2 files changed, 8 insertions(+), 1 deletion(-)
EOF
diff $L/trees.expected $L/trees.txt && echo TREE-OK
```

Expected `TREE-OK`. Then the files the runs depend on:

```sh
D=/var/tmp/task-1123-rel4; L=/var/log/task-1123-rel4; P=tests/zfs-tests/tests/functional
cat > $L/sha.txt <<'EOF'
21ff13635dcb79841c0a437653fa8e954f24ae80757fbda747b3f29e3ebcdfbe  pnr/module/zcommon/zfs_namecheck.c
c772bde2a5b83644e11a14aec1afce92cada14e0d4ad66077d706b757dbfc388  pnr/lib/libzfs/libzfs_pool.c
8af92cdccb2cc468c594f8a1c94670e3d1e31518b64b5682cf53ec6db86834f8  pnr/tests/zfs-tests/tests/functional/pool_names/pool_names.kshlib
54a67e94e599def89c08bb3ec9c628d0d5a3433f21cace91ae429c890fc829dd  pnr/tests/zfs-tests/tests/functional/pool_names/pool_names_reserved_anyraid.ksh
1f3697210ce01f20cd643bc333baf352f1bc16bd68edba8937fd80f974cd9d25  rt1/tests/zfs-tests/tests/functional/cli_root/zhack/zhack_metaslab_leak.ksh
0630fb7cf212c8729c3d095bd3efa1b70ce288ab9b81838801593b28e9735269  rt1/tests/zfs-tests/tests/functional/trim/autotrim_off_exit.ksh
85a0796aafce27650668fbcddb79bbf4c4db1385ce9d5068260d6422b12ad00c  rt1/tests/runfiles/linux.run
82ef2a158d8b41133e27e0aae5eaf218c04936e99c83ce62e12fedd1a31a8f70  rt3/tests/zfs-tests/tests/functional/cli_root/zpool_initialize/zpool_initialize_detach_load.ksh
98068773640696ce400b864fd3421b62889d3f1847d3468b3ee49d77f3917509  rt3/tests/zfs-tests/tests/functional/cli_root/zpool_trim/zpool_trim_detach_load.ksh
a9de4a5a5480c85d5f2404f84127a948cea81509353b7f790fb34c1ea42f50bd  rt3/tests/zfs-tests/tests/functional/gang_blocks/gang_blocks_header_read_fail.ksh
9285e8b8bce02068ec893186f46ef09d9e12fce9808ceaba38ab3896d5826bc7  rt3/tests/zfs-tests/include/tunables.cfg
c65e6af0cacfea771fecd35013b28bc135e59a350f58b06adcd84a599c693ccd  zpm/cmd/zinject/zinject.c
4be1bdd8aa772d6966aeeb79230eab3b907563a9d094e42e4f93beeaa5dcc0cb  zpm/tests/zfs-tests/tests/functional/cli_root/zinject/zinject_args.ksh
EOF
cd $D && sha256sum -c --quiet $L/sha.txt && echo SHA-OK
{ cd $D/pnr && git ls-tree HEAD $P/pool_names/pool_names_reserved_anyraid.ksh
  cd $D/rt1 && git ls-tree HEAD $P/cli_root/zhack/zhack_metaslab_leak.ksh $P/trim/autotrim_off_exit.ksh
  cd $D/rt3 && git ls-tree HEAD $P/cli_root/zpool_initialize/zpool_initialize_detach_load.ksh $P/cli_root/zpool_trim/zpool_trim_detach_load.ksh $P/gang_blocks/gang_blocks_header_read_fail.ksh
  cd $D/zpm && git ls-tree HEAD $P/cli_root/zinject/zinject_args.ksh; } | cut -c1-6 | tr '\n' ' '; echo
echo "zpm $(grep -c '"panic (-p)");' $D/zpm/cmd/zinject/zinject.c) $(grep -c '"import|export delay (-P)");' $D/zpm/cmd/zinject/zinject.c)"
```

Expected `SHA-OK` (the four `pnr` sums are those TASK-1117-PNR checked for tree A), then seven times `100755`, then `zpm 1 1` (the panic branch names `-p`; only the `-P` branch keeps the `-P` label; master has `0 2`).

Any other difference: stop and report the outputs as they are. Edit no file of the trees.

## Step 2: the kit

Write the file below byte for byte (4-space indentation, no tab, a newline after the last line) to `$L/runs.sh`, with `cat > /var/log/task-1123-rel4/runs.sh <<'EOF'` ... `EOF` (the quoted delimiter keeps everything literal). Then:

```sh
L=/var/log/task-1123-rel4
echo "6e4b69f65ea199f164d25943459e19f0c5590f323fddbf604d25bedc7d5861ef  $L/runs.sh" | sha256sum -c && sh -n $L/runs.sh && echo KIT-OK
```

Expected an `OK` line and `KIT-OK`. If the sum differs, write the file again from this text (an HTML entity left in it is the usual cause); a second mismatch: stop and report.

`runs.sh <tree> <tag> <runs> <selection>` is TASK-1123-CCM3's runner with this task's `D`, `L` and leftover names. It runs as the login user, from `$D/<tree>`. It runs `zfs-tests.sh -vKOD <selection>` `<runs>` times, one at a time, each after the pre-run checks (the srcversion one against `$L/sv-<tree>`) and after a kernel mark `task-1123-rel4: <tag>-<n> start`. Each run's console goes to `$L/zts-<tag>-<n>.log`, and `$L/runs.txt` gets a line `<tag>-<n> rc=<rc> <seconds> s dir=<result dir>` followed by that run's `Test:` lines (path from `functional/`, duration, result).

```sh
#!/bin/sh
# TASK-1123-REL4 ZTS runner, run as the login user:
# runs.sh <tree> <tag> <runs> <zfs-tests.sh selection>
D=/var/tmp/task-1123-rel4
L=/var/log/task-1123-rel4
tree=$1
tag=$2
runs=$3
shift 3
out=$L/runs.txt
T=$D/$tree
cd $T || exit 1
echo "START $tag $tree $(date -u +%FT%TZ) $*" >> $out
n=1
while [ $n -le $runs ]; do
    sv=$(cat /sys/module/zfs/srcversion 2>&1)
    pools=$(timeout 30 sudo -n $T/zpool list -H -o name 2>&1)
    inj=$(timeout 30 sudo -n $T/zinject 2>&1 | head -n 1)
    loops=$(sudo -n losetup -a 2>&1)
    left=$(ls -d /var/tmp/file-vdev* /var/tmp/tunable-* /var/tmp/testdir \
        /var/tmp/pool_names_reserved /var/tmp/trim-vdev* \
        /var/tmp/zpool_initialize_detach_load.* \
        /var/tmp/zpool_trim_detach_load.* 2>/dev/null)
    if [ "$sv" != "$(cat $L/sv-$tree)" ] || [ -n "$pools" ] ||
        [ "$inj" != "No handlers registered." ] || [ -n "$loops" ] ||
        [ -n "$left" ] || [ -s /etc/zfs/zpool.cache ]; then
        echo "STOP before $tag-$n: srcversion=[$sv] pools=[$pools] zinject=[$inj] loops=[$loops] left=[$left] cache=[$(ls -l /etc/zfs/zpool.cache 2>&1)]" >> $out
        exit 1
    fi
    echo "task-1123-rel4: $tag-$n start" | sudo -n tee /dev/kmsg > /dev/null
    s=$(date +%s)
    sudo -n -u zts ./scripts/zfs-tests.sh -vKOD "$@" \
        > $L/zts-$tag-$n.log 2>&1 < /dev/null
    rc=$?
    R=$(awk '/^Log directory/ { print $3 }' $L/zts-$tag-$n.log)
    echo "$tag-$n rc=$rc $(( $(date +%s) - s )) s dir=$R" >> $out
    grep -E 'Test( \([^)]*\))?: .* \[[A-Z]+\]$' $L/zts-$tag-$n.log |
        sed -E -e 's|^.*/tests/functional/|    |' -e 's/  +/ /g' >> $out
    n=$((n + 1))
done
echo "DONE $tag $(date -u +%FT%TZ)" >> $out
```

## Step 3: the four release builds, two at a time (20 to 25 minutes)

First pair, `pnr` and `rt1`, 4 jobs each:

```sh
D=/var/tmp/task-1123-rel4; L=/var/log/task-1123-rel4
for t in pnr rt1; do setsid nohup sh -c "date -u +%T; cd $D/$t && ./autogen.sh && ./configure && nice make -j4; echo build-rc=\$?; date -u +%T" > $L/build-$t.log 2>&1 < /dev/null & done
```

Poll `grep -H build-rc= $L/build-pnr.log $L/build-rt1.log` until there are two lines; each must read `build-rc=0`. Then the second pair, the same command with `for t in rt3 zpm`, polled the same way. A build that fails: quote the last 40 lines of its log and stop once the other build of its pair has ended (do not start the next pair).

Then, for the four trees:

```sh
D=/var/tmp/task-1123-rel4; L=/var/log/task-1123-rel4
for t in pnr rt1 rt3 zpm; do echo "$t $(head -n 1 $L/build-$t.log) $(tail -n 1 $L/build-$t.log) $(grep -c 'warning:' $L/build-$t.log) $(grep 'warning:' $L/build-$t.log | grep -vc objtool)"; done
for t in pnr rt1 rt3 zpm; do
  echo "$t [$(grep -m1 '^  \$ ./configure' $D/$t/config.log | sed 's/ *$//')] $(grep '^DEBUG_ZFS=' $D/$t/config.log) $(grep -m1 -o 'whether assertion support will be enabled\.\.\. [a-z]*' $L/build-$t.log)"
done > $L/release.txt
cat > $L/release.expected <<'EOF'
pnr [  $ ./configure] DEBUG_ZFS='_without_debug' whether assertion support will be enabled... no
rt1 [  $ ./configure] DEBUG_ZFS='_without_debug' whether assertion support will be enabled... no
rt3 [  $ ./configure] DEBUG_ZFS='_without_debug' whether assertion support will be enabled... no
zpm [  $ ./configure] DEBUG_ZFS='_without_debug' whether assertion support will be enabled... no
EOF
diff $L/release.expected $L/release.txt && echo REL-OK
for t in pnr rt1 rt3 zpm; do /usr/sbin/modinfo -F srcversion $D/$t/module/zfs.ko > $L/sv-$t; echo "$t $(cat $L/sv-$t)"; done
```

Expected:
- per tree, its start and end times (quote the durations), the count of `warning:` lines and the count outside objtool notes (0 expected). Quote every `warning:` line that is not an objtool note: a release build has no `-Werror`, so a warning does not stop it;
- `REL-OK`: in each `config.log` the configure line is `  $ ./configure`, with nothing after it, and `DEBUG_ZFS='_without_debug'`; the configure output says `checking whether assertion support will be enabled... no`. Quote `$L/release.txt`. A tree with `--enable-debug` in its configure line, `_with_debug` or `yes` is not a release build: stop and report;
- the srcversions: `pnr A1807D494E76F86869B2426`, and `65D8C0C24A9B688639F0721` for `rt1`, `rt3` and `zpm` (see "Why"). A different value, or `rt1`, `rt3` and `zpm` not all equal, is recorded, not a stop.

## Step 4: the runs

**Start the kernel-log follower** once, before the first load:

```sh
L=/var/log/task-1123-rel4
setsid nohup sudo -n sh -c "echo \$\$ > $L/follow.pid; exec dmesg --follow > $L/dmesg-follow.txt" > /dev/null 2>&1 < /dev/null &
sleep 2; cat $L/follow.pid; pgrep -af 'dmesg --follow'
```

**Loading tree `<t>`** (at the start of 4a to 4d):

```sh
D=/var/tmp/task-1123-rel4; L=/var/log/task-1123-rel4; t=<tree>
lsmod | grep -E '^(zfs|spl) '
sudo -n $D/$t/scripts/zfs.sh; echo "load rc=$?"
echo "$t loaded=$(cat /sys/module/zfs/srcversion) built=$(cat $L/sv-$t) zfs_flags=$(cat /sys/module/zfs/parameters/zfs_flags)"
sudo -n dmesg | grep 'ZFS: Loaded module' | tail -n 1
echo "task-1123-rel4: $t loaded" | sudo -n tee /dev/kmsg > /dev/null; echo "$(date -u +%FT%TZ) $t loaded" >> $L/progress.md
```

Expected: the `lsmod` line prints nothing; `load rc=0`; `loaded` equals `built`; `zfs_flags=0`; the load line without "(DEBUG mode)", expected `ZFS: Loaded module v2.4.99-1, ZFS pool version 5000, ZFS filesystem version 5`. With "(DEBUG mode)" or a `zfs_flags` other than 0, a debug module is loaded: unload it, stop and report. As the `rt1`, `rt3` and `zpm` modules have the same srcversion, the srcversion check cannot tell them apart: always unload the previous tree's module and load the tree's own.

**A run's own output file:** for the run `<tag>-<n>` of test `<T>`, the test's `merged` file (or its `stdout` where there is no `merged`, as on VM 107 in TASK-1121-JBIS):

```sh
L=/var/log/task-1123-rel4; r=<tag>-<n>; T=<test>
R=$(sed -n "s/^$r rc=.* dir=//p" $L/runs.txt); f=$(find "$R" -path "*/$T*" -type f \( -name merged -o -name stdout \) | sort | head -n 1); echo "$r $f"
```

The greps of 4a to 4d run on `"$f"`. Copy each run's file to `$L/out-<tag>-<n>.txt`.

**After a tree's runs** (at the end of 4a to 4d):

```sh
D=/var/tmp/task-1123-rel4; L=/var/log/task-1123-rel4; t=<tree>
grep -E "^(START|DONE) $t-|^STOP" $L/runs.txt; cat $L/runs-$t.nohup
timeout 30 sudo -n $D/$t/zpool list -H -o name; timeout 30 sudo -n $D/$t/zinject | head -n 1; sudo -n losetup -a
sed -n "/task-1123-rel4: $t loaded/,\$p" $L/dmesg-follow.txt | grep -E 'VERIFY|ASSERT|PANIC|BUG:|Oops|general protection|Call Trace|blocked for more than|possible circular|deadlock|KFENCE|KASAN|segfault|WARNING'
```

Expected: no STOP line, an empty nohup file, no pool, `No handlers registered.`, no loop device, and no kernel line. Only then (and not in the two cases of step 4c), unload:

```sh
D=/var/tmp/task-1123-rel4; L=/var/log/task-1123-rel4; t=<tree>
sudo -n $D/$t/scripts/zfs.sh -u; echo "unload rc=$?"; lsmod | grep -E '^(zfs|spl) '
echo "$(date -u +%FT%TZ) $t unloaded" >> $L/progress.md
```

Expected `unload rc=0` and nothing from `lsmod`. Then quote, for the tree's tags:

```sh
L=/var/log/task-1123-rel4; t=<tree>
grep -E "^$t-[a-z0-9]+-[0-9]+ rc=" $L/runs.txt
awk -v t="$t" '/^START / { on = ($2 ~ "^" t "-") } on && /\[[A-Z]+\]$/' $L/runs.txt
```

Every `rc=0`, and every `Test:` line `[PASS]`. Quote them all (with their durations).

### 4a. `pnr` (04-d): the new test 3 times, the group, two single tests

Load `pnr`. Then:

```sh
L=/var/log/task-1123-rel4
setsid nohup sh -c "sh $L/runs.sh pnr pnr-new 3 -t pool_names_reserved_anyraid && sh $L/runs.sh pnr pnr-grp 1 -T pool_names && sh $L/runs.sh pnr pnr-c007 1 -t zpool_create_007_neg && sh $L/runs.sh pnr pnr-c005 1 -t zpool_create_anyraid_005_neg" > $L/runs-pnr.nohup 2>&1 < /dev/null &
```

Poll `tail -n 3 $L/runs.txt` until `DONE pnr-c005` or a `STOP` line (about 3 minutes). Expected, all `[PASS]`:
- `pnr-new-1` to `-3`: `pool_names/pool_names_reserved_anyraid` (the directory has no setup or cleanup), a few seconds each (PNR: 00:01 to 00:02);
- `pnr-grp-1`: `pool_names/pool_names_001_pos`, `pool_names_002_neg` and `pool_names_reserved_anyraid` (PNR: 00:24, 00:08, 00:02);
- `pnr-c007-1` and `pnr-c005-1`: `cli_root/zpool_create/setup`, the test, `cli_root/zpool_create/cleanup`.

For `pnr-new-1` (`T=pool_names_reserved_anyraid`), quote `grep -E 'SUCCESS: (zpool create -m none any|zpool import -d [^ ]+ (testpool1 )?any|zhack label repair -c )' "$f"`. Expected, in this order (PNR's FA and the text's lines):

```
SUCCESS: zpool create -m none anymirror /var/tmp/pool_names_reserved/vdev exited 1
SUCCESS: zpool import -d /var/tmp/pool_names_reserved/vdev2 testpool1 anymirror exited 1
SUCCESS: zhack label repair -c /var/tmp/pool_names_reserved/vdev
SUCCESS: zpool import -d /var/tmp/pool_names_reserved/vdev anymirror
SUCCESS: zpool import -d /var/tmp/pool_names_reserved/vdev anymirror
SUCCESS: zpool create -m none anyraid /var/tmp/pool_names_reserved/vdev exited 1
SUCCESS: zpool import -d /var/tmp/pool_names_reserved/vdev2 testpool1 anyraid exited 1
SUCCESS: zhack label repair -c /var/tmp/pool_names_reserved/vdev
SUCCESS: zpool import -d /var/tmp/pool_names_reserved/vdev anyraid
SUCCESS: zpool import -d /var/tmp/pool_names_reserved/vdev anyraid
```

For `pnr-new-2` and `-3`, the count of the same `grep -cE`: 10 each. Then "After a tree's runs" with `t=pnr`.

### 4b. `rt1` (10-f): each test 3 times

Load `rt1`. Then:

```sh
L=/var/log/task-1123-rel4
setsid nohup sh -c "sh $L/runs.sh rt1 rt1-trim 3 -t autotrim_off_exit && sh $L/runs.sh rt1 rt1-leak 3 -t zhack_metaslab_leak" > $L/runs-rt1.nohup 2>&1 < /dev/null &
```

Poll until `DONE rt1-leak` or a `STOP` line (about 2 minutes). Expected, all `[PASS]`:
- `rt1-trim-1` to `-3`: `trim/setup`, `trim/autotrim_off_exit` (RT1: 00:02), `trim/cleanup`. A `[SKIP]` of the test means the trim setup found no discard support on the loop devices: quote the setup's `DISKS do not support discard` line from its own output file (`T=setup`, within the run's `dir=`), say so, and go on; it is no result;
- `rt1-leak-1` to `-3`: `cli_root/zhack/zhack_metaslab_leak` (RT1: 00:11).

Per run, quote `grep -E 'NOTE: .*(vdev_autotrim threads after|leaked segments)' "$f"`, with `T=autotrim_off_exit` or `T=zhack_metaslab_leak`. Expected for each trim run three lines, in this order, K small (RT1: 0, 2, 0):

```
NOTE: 2 vdev_autotrim threads after K syncs
NOTE: 0 vdev_autotrim threads after K syncs
NOTE: 2 vdev_autotrim threads after K syncs
```

and for each leak run one line, `NOTE: N leaked segments: block traversal size X != alloc Y (leaked Z)` (RT1: N 107 to 122, Z about 134 400 000). Then `pgrep -x vdev_autotrim | wc -l`: 0. Then "After a tree's runs" with `t=rt1`.

### 4c. `rt3` (11-g): each test 3 times

Load `rt3`. Then:

```sh
L=/var/log/task-1123-rel4
setsid nohup sh -c "sh $L/runs.sh rt3 rt3-init 3 -t zpool_initialize_detach_load && sh $L/runs.sh rt3 rt3-trim 3 -t zpool_trim_detach_load && sh $L/runs.sh rt3 rt3-gang 3 -t gang_blocks_header_read_fail" > $L/runs-rt3.nohup 2>&1 < /dev/null &
```

Poll until `DONE rt3-gang` or a `STOP` line (about 5 minutes). Expected, all `[PASS]`, each test between its group's setup and cleanup:
- `rt3-init-1` to `-3`: `cli_root/zpool_initialize/zpool_initialize_detach_load` (RT3: 00:17 to 00:18);
- `rt3-trim-1` to `-3`: `cli_root/zpool_trim/zpool_trim_detach_load` (RT3: 00:17). A `[SKIP]` (no discard support, as in 4b): quote the setup's line and go on;
- `rt3-gang-1` to `-3`: `gang_blocks/gang_blocks_header_read_fail` (RT3: 00:07).

Per run, quote `grep -E 'NOTE: .*(space map of metaslab 0|loading_time|abdstats struct_size)' "$f"`. Expected:
- detach runs: `NOTE: space map of metaslab 0 of the mirror: N bytes`, N above 4096 (RT3: 5520 to 6360), then a `NOTE: metaslab_load: ...` line for `spa testpool, class normal, vdev_id 0, ms_id 0` whose `loading_time` is 5000 ms or more (RT3: 15301 to 15470: the load waits for the delayed reads of the special vdev). Quote the first run's `metaslab_load` line in full, then N and `loading_time` for the others;
- gang runs: `NOTE: abdstats struct_size B before 2000 failed reads, A after`, with A - B under 32000 (RT3: +448 each time; a release `abd_t` is smaller, so the numbers may differ).

Two failures change what follows:
- **A detach test that FAILs at `zpool detach did not return within 120 seconds`** is the bug on this build. Its cleanup leaves the hung pool behind (`NOTE: zpool detach did not return, leaving testpool`), so `runs.sh` writes a `STOP` before the next run. Do not unload (it would block): quote the failing lines and `sudo -n dmesg | tail -n 60`, capture as for a hang, stop and report.
- **A gang test that FAILs at `struct_size grew by N bytes`**: the module leaks an `abd_t` per failed read, and `rmmod zfs` panics on leaked `abd_t` (TASK-1117-RT3). Quote the two lines, do not unload, stop and report.

Then "After a tree's runs" with `t=rt3`.

### 4d. `zpm` (24): the refusals, then `zinject_args` 3 times

Load `zpm`. With the module loaded and no pool, zinject's option checks refuse these before any ioctl; the pool name does not exist and is never looked up. These are the cases the text says it checked on its fix:

```sh
D=/var/tmp/task-1123-rel4; L=/var/log/task-1123-rel4
for a in "-p f -f 50" "-p f -l 1" "-p f -r 0,1" "-p f -t data" "-p f -C 0" "-P import -f 50" "-p f -d rel4nodev"; do
  e=$(sudo -n $D/zpm/zinject $a rel4nopool 2>&1 > /dev/null); echo "rc=$? [$a] $e"
done | tee $L/zpm-cli.txt
cat > $L/zpm-cli.expected <<'EOF'
rc=2 [-p f -f 50] panic (-p) incompatible with other options
rc=2 [-p f -l 1] panic (-p) incompatible with other options
rc=2 [-p f -r 0,1] panic (-p) incompatible with other options
rc=2 [-p f -t data] panic (-p) incompatible with other options
rc=2 [-p f -C 0] panic (-p) incompatible with other options
rc=2 [-P import -f 50] import|export delay (-P) incompatible with other options
rc=2 [-p f -d rel4nodev] device (-d) incompatible with data error injection
EOF
diff $L/zpm-cli.expected $L/zpm-cli.txt && echo CLI-OK
timeout 30 sudo -n $D/zpm/zinject | head -n 1
```

Expected `CLI-OK` (zinject prints its usage on stdout, which goes to `/dev/null`; each message is the one line on stderr), then `No handlers registered.`. Master's zinject would print `import|export delay (-P) incompatible with other options` on the first five lines. Any other line: quote `$L/zpm-cli.txt`, say so, and go on.

Then:

```sh
L=/var/log/task-1123-rel4
setsid nohup sh -c "sh $L/runs.sh zpm zpm-args 3 -t zinject_args" > $L/runs-zpm.nohup 2>&1 < /dev/null &
```

Poll until `DONE zpm-args` or a `STOP` line (about a minute). Expected: `cli_root/zinject/zinject_args` `[PASS]` 3 times (the directory has no setup or cleanup). Per run (`T=zinject_args`), quote `grep -E 'zinject -p f |refused with' "$f"`. Expected the three refusals of `test_panic_fault_neg`, `<disk>` being the first of the run's loop devices:

```
SUCCESS: eval zinject -p f -d <disk> testpool exited 2
SUCCESS: eval zinject -p f -t data /testpool/zinject_args.file exited 2
SUCCESS: eval zinject -p f -f 50 testpool exited 2
```

and no `zinject -p -f refused with:` line (the new check prints nothing when it passes). Then "After a tree's runs" with `t=zpm`.

## Step 5: end and cleanup

- Stop the follower: `L=/var/log/task-1123-rel4; sudo -n kill "$(cat $L/follow.pid)"; sleep 1; pgrep -af 'dmesg --follow'` shows only what step 0 found. Keep `$L/dmesg-follow.txt`.
- `lsmod | grep -E '^(zfs|spl) '`: empty. `ls -l /dev/zfs 2>&1`: absent. `sudo -n losetup -a`: empty.
- `ls -l /etc/zfs/zpool.cache 2>&1`: absent or size 0, as in step 0. Above 0: report it and leave it; do not read it.
- `ls -A /var/tmp | diff /var/log/task-1123-rel4/vartmp-0.txt -`: quote it. Remove only what this task created there: `sudo -n rm -rf /var/tmp/task-1123-rel4`, and any `file-vdev*`, `zfs-tests.*.run`, `zts-results.*`, `zts-report.*` or `constrained_path.*` the runs left. `test_results` stays. Anything else new: quote it and leave it.
- `sudo -n fstrim -av`, `df -h /var/tmp`. Keep `/var/log/task-1123-rel4`.

## Report

Write `/var/log/task-1123-rel4/report.md` and give the same text as your final message:

1. **Environment:** VM and hostname, kernel, nproc, RAM, free space, the zed, follower, losetup, cache and sysctl lines of step 0; FETCH-OK for each tree (by SHA or by branch name), TREE-OK, SHA-OK, the modes, the `zpm` count line; KIT-OK; each build's rc, duration and warning counts (and any warning outside objtool notes); `release.txt` and REL-OK; the four srcversions; for each load the srcversion, `zfs_flags` and the load line.
2. **Table:** tree | tag | test | runs | result (PASS count, or each non-PASS with its failing line) | durations | NOTE values. Then one verdict line, for example: "release builds (no `--enable-debug`): pnr pool_names_reserved_anyraid 3/3, -T pool_names 3/3, zpool_create_007_neg and zpool_create_anyraid_005_neg PASS; rt1 autotrim_off_exit 3/3, zhack_metaslab_leak 3/3; rt3 zpool_initialize_detach_load 3/3, zpool_trim_detach_load 3/3, gang_blocks_header_read_fail 3/3; zpm CLI-OK, zinject_args 3/3; kernel log clean", or what differs.
3. **Values for the pull request texts,** one line each, exactly these keys (n/a where a step did not run):

   ```
   rel4.env      = VM <105|106> (<hostname>), Linux <uname -r>, <nproc> CPUs, <MiB> MiB
   rel4.builds   = pnr rc <rc> <mm:ss>, rt1 rc <rc> <mm:ss>, rt3 rc <rc> <mm:ss>, zpm rc <rc> <mm:ss>; warnings outside objtool: <none | the lines>
   rel4.release  = "$ ./configure", DEBUG_ZFS='_without_debug', assertions no: <all four | what differs>
   rel4.sv       = pnr <SV>, rt1 <SV>, rt3 <SV>, zpm <SV>
   rel4.load     = "<load line>", zfs_flags <value> (<all four | what differs>)
   pnr.new       = pool_names_reserved_anyraid <PASS>/3 (<durations>); the 10 lines <as expected | what differs>
   pnr.group     = -T pool_names <PASS>/3 (<durations of 001_pos, 002_neg, reserved_anyraid>)
   pnr.single    = zpool_create_007_neg <result> (<d>); zpool_create_anyraid_005_neg <result> (<d>)
   pnr.kernel    = <none | the lines>
   rt1.trim      = autotrim_off_exit <PASS>/3 (<durations>; K <k,k,k> | <k,k,k> | <k,k,k>)
   rt1.leak      = zhack_metaslab_leak <PASS>/3 (<durations>; <N> leaked segments, leaked <Z>, per run)
   rt1.threads   = vdev_autotrim threads left: <n>
   rt1.kernel    = <none | the lines>
   rt3.init      = zpool_initialize_detach_load <PASS>/3 (<durations>; space map <min> to <max> bytes; loading_time <min> to <max> ms)
   rt3.trim      = zpool_trim_detach_load <PASS>/3 (<durations>; space map <min> to <max> bytes; loading_time <min> to <max> ms)
   rt3.gang      = gang_blocks_header_read_fail <PASS>/3 (<durations>; struct_size +<A-B> per run)
   rt3.kernel    = <none | the lines>
   zpm.cli       = CLI-OK <yes | the lines that differ>
   zpm.args      = zinject_args <PASS>/3 (<durations>); the three -p refusals exited 2 in each run: <yes | what differs>
   zpm.kernel    = <none | the lines>
   ```

4. **Quotes:** the START/DONE lines, every `Test:` line, the greps of 4a to 4d (the first run of each tag in full, then what differs), `zpm-cli.txt`, every non-PASS with its lines, the kernel-line checks.
5. **Deviations, cleanup,** then `End`.
