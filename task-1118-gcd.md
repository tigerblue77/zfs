TASK-1118-GCD (from the coordinator session "main"): performance and correctness, VM 105 (CHSONVTST02) on 7.0.14-25-pve, about 3 h on the VM plus the wait for main's GO, at most 3 h 30 without that wait. Its measured part (step 4, about 95 min, never more than 115; about 75 without the CCM runs) runs ALONE on the host and on a quiet VM: before it you end your turn with a note for main and wait for main's GO; you never check the other VMs yourself. Steps 0 to 2 are functional and may run while other VMs work. Three arms on TASK-1118-K35's encrypted-read workload: upstream master, our upstream PR #19776 (the base), and our branch that decrypts AES-GCM and AES-CCM straight into the output. Same VM rules as before: nothing published on GitHub, no credential files (the scratch pool's test key below is not one: it protects random test data and is deleted at END), fstrim after deletions. Reboot when needed without waiting for main (save the diagnostics first: dmesg, stacks, logs under /var/log/task-1118-gcd/), arrange your own resumption as K35 and K35B did, and report the reboot. Report: `/var/log/task-1118-gcd/report.md` and the same text as your final message.

**Your messages to main are blocked:** a message you send to main is refused, so do not try. Main reads your chat instead. Every note for main is therefore the end of a turn in your own chat. End this task with your final report as your last message (the full text, nothing after it), then stay idle until main writes. If you reboot at the end (you will, see END), that last message is the report sent after the reboot.

The transport may show the characters greater-than, less-than and ampersand as HTML entities: read them as the characters.

Shell variables do not survive from one of your commands to the next: start each command with the assignments it needs (`L`, `D`); the blocks below do.

## Why

K35 (this VM, the same workload: aes-256-gcm, recordsize=1M, 600 s of reads of `/tank/enc/f`) found on master about 1200–1300 kswapd wake-ups of order 8 per run (1296 / 1194), and every wake-up of order >= 4 came from `gcm_mode_decrypt_contiguous_blocks()` in z_rd_int: about half from the kmalloc attempt of `spl_kvmalloc()` (class K, the 1 MiB buffer the ciphertext is gathered in) and half from the vmalloc of `spl_kmem_alloc_impl()` above spl_kmem_alloc_max (class VD, the 1 MiB + 16 buffer it is regrown into for the tag). Mean `c` was 2.6–2.9 GiB on master, against 5.4–5.5 GiB in the arms without those wake-ups. K35B tested the SPL-side remedy (#19935), which keeps the allocations and stops them waking kswapd.

The branch removes the allocations instead. On master, for a 1 MiB block, the decryption allocates 1 MiB, then 1 MiB + 16 bytes, and copies 3 MiB; `gcm_decrypt_final()` decrypts in that buffer, copies the plaintext out once the tag matches, then the buffer is zeroed (1 MiB) and freed. Arm **G** (fork branch `icp-gcm-decrypt-no-tmpbuf`, 5824a830cae, 4 commits on B) keeps a pointer to the ciphertext when it arrives in one piece, and decrypts straight into the output when the plaintext range there is contiguous and does not overlap the ciphertext: no allocation, 16 bytes copied (the tag). zio_crypt decrypts every block but dnode and ZIL blocks this way. When the tag does not match, G zeroes that output range and returns the error, so no unauthenticated plaintext is left there. AES-CCM, which on master allocates 1 MiB and copies 1 MiB per 1 MiB decryption, gets the same. The other layouts (dnode and ZIL blocks, wrapped keys) are still gathered, now in one allocation of the full size: `zfs load-key` and every mount and object lookup in this task go through that path.

Arm **B** (fork branch `icp-gcm-direct-output-best`, 947ad70cd5e: two commits on master) is the version of our upstream PR #19776 that G is built on. It changes only the AES-GCM encryption path and crypto_test, so on this read-only workload it should match M; it is the base the PR is measured against. Fetch it from the fork branch: `refs/pull/19776/head` on openzfs still holds an older version.

Questions: (1) does G remove the wake-ups of order >= 4 and the vmap areas above 128 KiB of the decrypt path (expected: all of them); (2) against B, does G raise read MB/s, lower the CPU time per GiB read, raise mean `c`, lower kswapd runs and direct reclaim, beyond the spread of each arm; (3) does B match M; (4) is G correct: every record reads back as written, a block whose authentication fails reads as EIO and not as data, and `crypto_test` passes on every AES-GCM and AES-CCM implementation this CPU has; (5) for AES-CCM, one run per arm, as an indication.

## Arms

| Arm | Tree | Commit (full SHA) | Tree hash | `git diff --numstat c913da86156eba3243cc1ef515e98d69887654a7` | spl srcversion | zfs srcversion |
| --- | --- | --- | --- | --- | --- | --- |
| M | `~/work/z-M0` (exists, built) | c913da86156eba3243cc1ef515e98d69887654a7 | 7e32938edf1a54c02519e2dc0104e9325b5a6f4d | (none) | BA40B6EA1E022CA48AB235F | DC89AB9376CF0B2D8E0FB13 |
| B (#19776) | `~/work/z-B` (new) | 947ad70cd5e416e0bbcb479a226e86b958ae4d73 | 6c79b2c7760bcbb65ee7b7bf67d6ae4c8e79e51b | gcm.c 68/16, crypto_test.c 2/2 | BA40B6EA1E022CA48AB235F | 2EFBCD6C96502ECF9AB9F01 |
| G (B + decrypt into the output) | `~/work/z-G` (new) | 5824a830cae9103965cdc834c0d5efd8b3813256 | 7295ab787baddcb30d044b5ef443244afae87937 | ccm.c 112/13, gcm.c 161/102, modes.c 35/1, modes.h 26/6, aes.c 6/1, crypto_test.c 29/3 | BA40B6EA1E022CA48AB235F | 68245B2ECE7DA614EEE27A4 |

- The files are `module/icp/algs/modes/{ccm,gcm,modes}.c`, `module/icp/include/modes/modes.h`, `module/icp/io/aes.c` and `tests/zfs-tests/cmd/crypto_test.c`; their sha256 per tree are in the check lists of step 1.
- B's parent is 32807a63bdb202537b8a93a876ecca260a776c9a ("crypto_test: run the AVX2 AES-GCM code"), whose parent is c913da86156. G is four commits on B: 84cae79c890 (gather the ciphertext in one allocation), 15ddbc7e793 (GCM direct output), 5e113e583c5 (CCM direct output), 5824a830cae (crypto_test: a failed decryption leaves no plaintext). Author of all six: Tigerblue77.
- No arm changes the SPL: the spl srcversion is the same in all three, and the three zfs srcversions must differ. B's and G's values were computed from builds of the same commits against the 6.17 headers, which give M's BA40B6EA1E022CA48AB235F and DC89AB9376CF0B2D8E0FB13 as on this VM: a different value is a stop.
- The other trees under `~/work` are not used and stay untouched.

## Step 0: state

```sh
L=/var/log/task-1118-gcd; D=/var/tmp/task-1118-gcd
date -u; uname -r; cat /proc/cmdline; free -m; df -h / /var/tmp ~/work
lsmod | grep -E '^(zfs|spl) '; ls -d $L $D ~/work/z-B ~/work/z-G /gcdscr 2>&1
ls -d ~/work/z-M0 /usr/src/linux-headers-7.0.14-25-pve /var/log/task-1116-ar/files.tar.gz /var/log/task-1118-k35/files.tar.gz /var/log/task-1118-k35b/report.md
ls -l /var/log/task-1118-k35b/files.tar.gz 2>&1
ps -eo pid=,comm= | grep -wE 'fio|make|cc1|ztest|zfs-tests.sh|zloop.sh|configure|t2.sh|seq.sh'
```

Stop and report, changing nothing, if: `uname -r` does not start with `6.12.`; a zfs or spl module is loaded; `$L`, `$D`, `~/work/z-B`, `~/work/z-G` or `/gcdscr` exists; less than 20 GiB free on the build filesystem; one of the paths of the third line is missing (K35B's report among them: that task must be over); a build, test or read job runs. K35B's archive may be missing: see "AR's scripts" in step 1.

## Step 1: kit, fetch, worktrees, builds (release)

**Kit,** the same as K35 and K35B (fork branch `vm-kit-k35`, one commit with no parent holding exactly three files):

```sh
L=/var/log/task-1118-gcd; D=/var/tmp/task-1118-gcd
mkdir -p $L $D/kit && cd $D/kit && git init -q .
git fetch -q --depth 1 https://github.com/tigerblue77/zfs vm-kit-k35
git rev-parse FETCH_HEAD                        # 49713644a8208044f6ab196525d7a79eb0e4e926
git rev-parse 'FETCH_HEAD^{tree}'               # a88691d356b65e20e0c3bb2dbb75840e3404718b
git cat-file -p FETCH_HEAD | grep -c '^parent'  # 0
git checkout -q FETCH_HEAD
sha256sum k35-size.sh k35-reduce.py k35-sum.py
```

Expected sha256: `k35-size.sh` e57bd7e50e90ef60ca04925915216dbeed17981f421072c5291b48df21f9bf75, `k35-reduce.py` 55b3df289db97ba959400a394fc0d97a32874cfce28ed3c93b1bcf25ac2cbd3d, `k35-sum.py` 26f5681ea02ec05fbf1f0ec31fee1225f7c31d0194c519b1a37ff817b9e294bc. If one differs, stop and report. `k35-sum.py` takes the arm as the run label up to its last `-`, so `CG-1` is arm CG.

**Check lists** of the six changed files, one per arm, in `sha256sum -c` format:

```sh
L=/var/log/task-1118-gcd
cat > $L/sums-M.txt <<'EOF'
a47b5b4c4e2e5256eae6358bd47b7e269a9fb470ef9d56d1a9765a0b788e937b  module/icp/algs/modes/gcm.c
2940be65c72cc00bc8fc19b247d1bf5b7e5d875d661b12b345f8a25b4e403ea1  module/icp/algs/modes/ccm.c
b8c7b20c5ca910ceeed6f0668ebacc091e6eb6532366cd232b99de09ad20cad7  module/icp/algs/modes/modes.c
557fd7bca9c446fe504e1abe887f8098efd531ccafa422668765a6fba7da9be8  module/icp/include/modes/modes.h
5f246b899c204114c8cdf14260cc4604567df4612a81a917402ef5bc35b66b74  module/icp/io/aes.c
894c96131cbb147dc7ef9e604b9937949d68fa3cb68f4c59c313a2431861d436  tests/zfs-tests/cmd/crypto_test.c
EOF
cat > $L/sums-B.txt <<'EOF'
8b9d39c3d0a778502a5edf6094a5c8c2012ad13c9c48c464fcfc5cab994de69c  module/icp/algs/modes/gcm.c
2940be65c72cc00bc8fc19b247d1bf5b7e5d875d661b12b345f8a25b4e403ea1  module/icp/algs/modes/ccm.c
b8c7b20c5ca910ceeed6f0668ebacc091e6eb6532366cd232b99de09ad20cad7  module/icp/algs/modes/modes.c
557fd7bca9c446fe504e1abe887f8098efd531ccafa422668765a6fba7da9be8  module/icp/include/modes/modes.h
5f246b899c204114c8cdf14260cc4604567df4612a81a917402ef5bc35b66b74  module/icp/io/aes.c
69baf406e9dbe1bde43bc12810d0ce86688e0f43cb94332aa0eff0fd789541c1  tests/zfs-tests/cmd/crypto_test.c
EOF
cat > $L/sums-G.txt <<'EOF'
16794c73f233e28ce5ee18a8a93e0644eec3f834d81b1810374671be51976538  module/icp/algs/modes/gcm.c
9a54db59b5bee51927b51b65fc4171e1fc322da240a198f0e967b628a3fdce6b  module/icp/algs/modes/ccm.c
fc45d644830c00e84492fe79fa2cd06815fd06728c327ff511b7427663749d65  module/icp/algs/modes/modes.c
24a07ff157a92c576899f45e8268e6c9fa9edfbae45dc5c47ee44a8f674b211c  module/icp/include/modes/modes.h
fa8be5ec6d2d0b51f7a23d72fd5dcd7ebe00b1665237b16440ffaf3b2e16f130  module/icp/io/aes.c
adcf91b9826fdd7111660126311afdddefd1ed7d2480fee24ff62f7a84f6be96  tests/zfs-tests/cmd/crypto_test.c
EOF
```

**M,** unchanged since K35B:

```sh
L=/var/log/task-1118-gcd; cd ~/work/z-M0
{ git rev-parse HEAD 'HEAD^{tree}'; git status --short --untracked-files=no; sha256sum -c $L/sums-M.txt; grep -m1 '^  \$ ./configure' config.log; modinfo -F srcversion module/spl.ko module/zfs.ko; } 2>&1 | tee $L/tree-M0.txt
```

Commit, tree hash, checksums and srcversions must be those of the table, and `git status` empty. If the tree is at the right commit but its built modules are missing, rebuild it as below and require the srcversions of the table; anything else that differs: stop and report.

**B and G,** fetched from the fork in the clone that holds the worktrees (z-M0 is one of them):

```sh
L=/var/log/task-1118-gcd; cd ~/work/z-M0
git fetch https://github.com/tigerblue77/zfs icp-gcm-direct-output-best
git rev-parse FETCH_HEAD 'FETCH_HEAD^{tree}'
git log -1 --format='%P | %an | %s' FETCH_HEAD
git fetch https://github.com/tigerblue77/zfs icp-gcm-decrypt-no-tmpbuf
git rev-parse FETCH_HEAD 'FETCH_HEAD^{tree}'
git log -1 --format='%P | %an | %s' FETCH_HEAD
git log --format='%H %T %P | %an | %s' c913da86156eba3243cc1ef515e98d69887654a7..5824a830cae9103965cdc834c0d5efd8b3813256 | tee $L/log.txt | sha256sum
for c in 947ad70cd5e416e0bbcb479a226e86b958ae4d73 5824a830cae9103965cdc834c0d5efd8b3813256; do echo "== $c"; git diff --numstat c913da86156eba3243cc1ef515e98d69887654a7 $c | tr '\t' ' ' | tee -a $L/numstat.txt | sha256sum; done
git worktree add --detach ~/work/z-B 947ad70cd5e416e0bbcb479a226e86b958ae4d73
git worktree add --detach ~/work/z-G 5824a830cae9103965cdc834c0d5efd8b3813256
```

Expected, in order:
- `947ad70cd5e416e0bbcb479a226e86b958ae4d73`, `6c79b2c7760bcbb65ee7b7bf67d6ae4c8e79e51b`, `32807a63bdb202537b8a93a876ecca260a776c9a | Tigerblue77 | ICP: AES-GCM: encrypt straight into a contiguous output`;
- `5824a830cae9103965cdc834c0d5efd8b3813256`, `7295ab787baddcb30d044b5ef443244afae87937`, `5e113e583c5ea19ed372dd473580cc7eca7495a8 | Tigerblue77 | crypto_test: check that a failed decryption leaves no plaintext`;
- the six-commit log: sha256 `e01cfb27faeb04ff4dfed1f58a1ba23e789709b85d279e1acf89a54550565337` (its oldest line is 32807a63bdb… with parent c913da86156…);
- numstat of B: `434df469c05159eb55a9581ff6f83f715008e690e116823ebb0ccf52224f8137`; of G: `761418339de879241d8cce79237c35d49d407205adf104eae0974a904b5566e8` (the lines of the table, tab replaced by a space).

If a fetch fails, wait 1 minute and try once more. If a FETCH_HEAD is another commit (a branch was pushed again) or any value differs, stop and report it, building nothing. Compare every value with the program, not by eye.

**Builds:** in each new tree, exactly as the other trees were built: `./autogen.sh`, configure with exactly the command line at the top of `~/work/z-M0/config.log` (K35: `./configure --with-linux=/usr/src/linux-headers-7.0.14-25-pve --with-linux-obj=/usr/src/linux-headers-7.0.14-25-pve`, so the builds can run before the reboot), `nice make -j8`. The two trees may build at the same time (then `-j4` each). Keep the outputs in `$L/{autogen,configure,make}-<arm>.log`. If your shell tool cannot wait for a build in the foreground, start that one command with the tool's own background mode and wait until it exits (no `&`, no `nohup`). Then check, per tree:

```sh
L=/var/log/task-1118-gcd
for a in B G; do cd ~/work/z-$a && echo "== $a" && {
diff <(grep -m1 '^  \$ ./configure' ~/work/z-M0/config.log) <(grep -m1 '^  \$ ./configure' config.log) && echo same-configure
grep 'kernel crypto API is available' $L/configure-$a.log
grep -n -i 'warning' $L/make-$a.log | grep -v -i objtool
git status --short --untracked-files=no
sha256sum -c $L/sums-$a.txt
ls -l tests/zfs-tests/cmd/crypto_test
modinfo -F srcversion module/spl.ko module/zfs.ko; } 2>&1 | tee $L/tree-$a.txt; done
```

Expected: `same-configure`; `checking whether kernel crypto API is available... no` (stop if `yes`: the ICP would not be the code under test); no compiler warning on the six files; `git status` empty; every checksum `OK`; crypto_test built; the srcversions of the table. Stop if two arms share a zfs srcversion.

**AR's scripts, as K35 and K35B used them.** Extract `/var/log/task-1116-ar/files.tar.gz` into `$D/ar` and make there the changes K35 made (its report, Deviations): the AR path `/var/tmp/task-1116-ar` replaced by `$D/ar` in t2.sh, summ.sh, analyze.sh and control.sh and nothing else in them; AR's old outputs removed from the extracted copy; fragmem rebuilt from fragmem.c with `gcc -O2`. Take **K35B's seq.sh** from `/var/log/task-1118-k35b/files.tar.gz` (`tar -tzf` names it) as the base: it reloads the modules before every run. If that archive or its seq.sh is missing, take K35's from `/var/log/task-1118-k35/files.tar.gz` and add K35B's change (a module reload before every run, the second run of an arm included); say which you took. Change only:
- the task paths (`task-1118-k35b` or `task-1118-k35` to `task-1118-gcd`);
- the arms, their trees and expected srcversions: M `~/work/z-M0`, B `~/work/z-B`, G `~/work/z-G`, and for the CCM runs CM, CB and CG on the same three trees; spl BA40B6EA1E022CA48AB235F in every arm, zfs as in the table;
- the run list, and per run its file and lengths (step 4);
- **per run, just before and just after t2.sh** (beside k35-size.sh and dr.sh): the `cpu` line of `/proc/stat` into `<outdir>/<run>.cpu`, the `/proc/diskstats` lines of tank's disks into `<run>.disk` (the kernel names of the leaf devices `zpool status -LP tank` lists, as whole disks or partitions as diskstats has them; say which), and the `VmallocUsed` line of `/proc/meminfo` into `<run>.vmalloc`; after t2.sh also a copy of `/proc/spl/kstat/zfs/arcstats` into `<run>.arcstats`; at each load, the contents of `/sys/module/zfs/parameters/icp_aes_impl` and `icp_gcm_impl` into `<run>.icp`;
- **after each run,** after the k35-size.sh and dr.sh stops, `check.sh <run> <file> <ref>` (step 2) with its output in `<outdir>/<run>.check`; seq.sh stops, running nothing more, if check.sh exits non-zero.

Keep `$D/kit/k35-size.sh start <outdir>/<run>` just before `t2.sh` and `$D/kit/k35-size.sh stop <outdir>/<run>` just after it, beside `dr.sh`, as in K35. Save the diff of the new seq.sh against its base as `$L/seq.diff` and quote it in the report.

**t2.sh's arguments.** Read t2.sh and say in the report what each argument does. The CCM runs use `t2.sh <run> enc /tank/ccm/f <tree> 300 10 300`: the GCM arguments with the two 600 values at 300. If those two arguments are not the read time and the trace time, keep 600 for the CCM runs and say so. If its `enc` mode names `tank/enc` anywhere other than through the file argument, change only that so that the CCM runs read `/tank/ccm/f`, and quote the diff.

## Step 2: boot, correctness checks, state

**Boot,** as in K35 and K35B: 7.0.14-25-pve once with `grub-reboot` and the menu entry they used (Debian stays the default), resuming by your own arrangement. After the boot: `uname -r` is 7.0.14-25-pve and `grub-editenv list` shows no next_entry. Then swapoff, lru_gen 0x0007, `vm.compact_unevictable_allowed=0` (note the values found). Quote `/proc/cmdline`; stop if it has slub_debug, kasan, kmemleak, page_poison, init_on_free or init_on_alloc=1.

**CPU,** for the implementations the ICP can use here:

```sh
L=/var/log/task-1118-gcd
{ grep -m1 '^model name' /proc/cpuinfo; nproc
for f in aes pclmulqdq avx avx2 movbe vaes vpclmulqdq avx512f avx512vl avx512bw; do grep -m1 '^flags' /proc/cpuinfo | grep -qw $f && echo "$f yes" || echo "$f no"; done; } 2>&1 | tee $L/cpu.txt
```

**crypto_test of G,** in user space (it links G's libzpool, so no module is needed). Every AES-GCM and AES-CCM implementation pair crypto_test knows, run on the test vectors of the tree; G's crypto_test also fills the output before each decryption and fails a vector whose failed decryption leaves anything but the fill or zeroes there:

```sh
L=/var/log/task-1118-gcd; cd ~/work/z-G
for a in gcm ccm; do ./tests/zfs-tests/cmd/crypto_test -v -c tests/zfs-tests/tests/functional/crypto/aes_${a}_test.txt; echo "rc $?"; done 2>&1 | tee $L/crypto_test-G.txt
```

Expected: one summary line per pair, 10 for AES-GCM (`generic`, `x86_64`, `aesni` with `generic` and `pclmulqdq`; `x86_64` and `aesni` with `avx` and `avx2-vaes`) and 3 for AES-CCM (`generic`, `x86_64`, `aesni`), each `passed=N failed=0` (the PR's run had 316 for GCM and 552 for CCM) or `[not supported on this platform]`, and `rc 0` twice. List the pairs not supported and check them against cpu.txt: `aesni` needs aes; `pclmulqdq` needs pclmulqdq; `avx` needs avx, aes and pclmulqdq; `avx2-vaes` needs avx2, vaes and vpclmulqdq. Stop if any vector fails, or if a pair is not supported although the CPU has its flags.

**ZTS crypto tests on G:** load G (`sudo -n ./scripts/zfs.sh` from `~/work/z-G`, both srcversions checked), quote `/sys/module/zfs/parameters/icp_aes_impl` and `icp_gcm_impl`, then `sudo -n -u zts ./scripts/zfs-tests.sh -vKO -T crypto` from `~/work/z-G` (never `-x`; it runs icp_aes_ccm and icp_aes_gcm). If user zts cannot run it from `~/work/z-G` (permissions), say so and skip it: crypto_test above runs the same vectors. Then unload (`sudo -n ./scripts/zfs.sh -u`).

**Kit smoke test,** once, after loading M and importing `tank` (as seq.sh does): `$D/kit/k35-size.sh start $D/smoke`, read 2 GiB of `/tank/enc/f` the way t2.sh reads it, `$D/kit/k35-size.sh stop $D/smoke`, then `python3 $D/kit/k35-sum.py $D/smoke.k35`. Expect: vmap areas counted in `>128K`, every E count 0, `$D/smoke.k35.err` empty, `overrun: 0` in `$D/smoke.k35.stats`, and no `instances/k35` left. If not, stop and report.

**rec.py,** the read-back tool: write this text to `$D/rec.py`. Its sha256 must be ba31a83e64e6c66f9fe0ff8786d5fec3c7d5a74e99553acdc3af1fcb1dfe6335. If it differs, compare your copy with the text for transport damage (entities, quotes), fix only that and say so; if it still differs, stop and report.

```python
#!/usr/bin/python3
"""rec.py FILE [REF]: BLAKE2b-128 of each 1 MiB record of FILE.

Without REF, print one hash per record (FILE '-' reads stdin).
With REF, a list printed by rec.py, pread() each record of REF, print a
line for each record that differs ('N different') or fails ('N errno E
text'), then 'records R size S match M different D errors E'; exit 1
unless every record matches.
"""
import hashlib
import os
import sys

R = 1048576


def h(b):
    return hashlib.blake2b(b, digest_size=16).hexdigest()


if len(sys.argv) == 2:
    f = sys.stdin.buffer if sys.argv[1] == '-' else open(sys.argv[1], 'rb')
    while True:
        b = f.read(R)
        if not b:
            break
        print(h(b))
    sys.exit(0)

ref = open(sys.argv[2]).read().split()
fd = os.open(sys.argv[1], os.O_RDONLY)
size = os.fstat(fd).st_size
m = d = e = 0
for i, want in enumerate(ref):
    try:
        b = os.pread(fd, R, i * R)
    except OSError as x:
        e += 1
        print(i, 'errno', x.errno, x.strerror)
        continue
    if h(b) == want:
        m += 1
    else:
        d += 1
        print(i, 'different')
print('records', len(ref), 'size', size, 'match', m, 'different', d,
      'errors', e)
sys.exit(0 if m == len(ref) else 1)
```

All the steps below use the tools of the loaded arm's tree, as seq.sh calls them, with `sudo -n` where needed; run rec.py as `sudo -n python3 -I $D/rec.py`.

**Reference of the GCM file,** read by M from disk: `zpool export tank`, `zpool import tank`, `zfs load-key -a`, `zfs mount -a`, then `rec.py /tank/enc/f > $D/ref-enc.txt`, timed. Its line count must be the file's size divided by 1048576 (12288 if the file is AE's 12 GiB); quote both and the time.

**CCM dataset,** in `tank`, if cheap. Quote `zfs list -o name,used,avail,recordsize,encryption,keyformat,keylocation -r tank` first; if `tank` has less than 16 GiB available, skip the CCM dataset and the CCM runs (4b) and say so. Otherwise create `tank/ccm` with `encryption=aes-256-ccm`, `recordsize=1M`, and the keyformat and keylocation of `tank/enc` (so that `zfs load-key -a` loads it); compression and atime inherit from tank (off, as for tank/enc: check). Write it from a stream whose records are hashed on the way, so that its reference does not depend on any decryption, with as many records as `/tank/enc/f`:

```sh
D=/var/tmp/task-1118-gcd; N=$(wc -l < $D/ref-enc.txt)
head -c $((N * 1048576)) /dev/urandom | sudo -n tee /tank/ccm/f | python3 -I $D/rec.py - > $D/ref-ccm.txt
```

Then `zpool sync tank`, export and import as above, and `rec.py /tank/ccm/f $D/ref-ccm.txt` on M, timed: every record must match. That read is also the time a CCM check takes.

**Scratch pool,** for the corrupted block. On `tank` a corrupted block is caught by the block checksum before any decryption, so the scratch datasets have `checksum=off`: the AES-GCM tag (AES-CCM MAC) is then the only check, and a corrupted block reaches the failing decryption, which on G is the path that zeroes its output. A file-backed pool in `$D`, outside the pool cache; the name contains no "testpool":

```sh
D=/var/tmp/task-1118-gcd
head -c 32 /dev/urandom > $D/scr.key && truncate -s 256M $D/scr.img
sudo -n zpool create -o cachefile=none -O compression=off -O atime=off gcdscr $D/scr.img
for m in gcm ccm; do sudo -n zfs create -o encryption=aes-256-$m -o keyformat=raw -o keylocation=file://$D/scr.key -o recordsize=1M -o checksum=off gcdscr/$m; done
head -c 4M /dev/urandom > $D/scr.data && python3 -I $D/rec.py $D/scr.data > $D/scr-ref.txt
for m in gcm ccm; do sudo -n cp $D/scr.data /gcdscr/$m/f; stat -c '%i %s' /gcdscr/$m/f; done
sudo -n zpool sync gcdscr; sudo -n zfs get -H -o name,value checksum,encryption gcdscr/gcm gcdscr/ccm
sudo -n zpool export gcdscr
```

(`zpool` and `zfs` above stand for the tree's tools, as everywhere.) For each dataset, find the block of record 1 (file offset 0x100000) with `zdb -e -p $D -ddddd gcdscr/<m> <inode>` (add `-K $D/scr.key` if zdb cannot show the block pointers without the key): the L0 line whose first field is `100000` has the DVA `0:<offset>:<asize>`, offset in hex. Quote the L0 lines. Flip one byte 4096 bytes into that block (the file vdev's data starts after 4 MiB of labels and boot area):

```sh
D=/var/tmp/task-1118-gcd; off=<hex offset of the DVA, without 0x>
python3 -c 'import sys; f = open(sys.argv[1], "r+b"); p = 4194304 + int(sys.argv[2], 16) + 4096; f.seek(p); c = f.read(1); f.seek(p); f.write(bytes([c[0] ^ 255]))' $D/scr.img $off
```

Then, still on M: `zpool import -d $D gcdscr`, `zfs load-key gcdscr/gcm`, `zfs load-key gcdscr/ccm`, `zfs mount -a`, and for each dataset `rec.py /gcdscr/<m>/f $D/scr-ref.txt`. Expected, exactly, for both:

```
1 errno 5 Input/output error
records 4 size 4194304 match 3 different 0 errors 1
```

with new `ereport.fs.zfs.authentication` events for gcdscr in `zpool events` (a `checksum` class instead means the block checksum caught it: say so). Then `zpool export gcdscr`. If M does not give exactly that, the setup is wrong: stop and report.

**check.sh,** in `$D`, the correctness check seq.sh runs after every run (`check.sh <run> <file> <ref>`), with the loaded arm's tools; it exits non-zero on any deviation and prints what it saw:
1. `zpool export tank`, `zpool import tank`, `zfs load-key -a`, `zfs mount -a`, so that the file is read from disk by the loaded arm;
2. `rec.py <file> <ref>`, timed: it must print only `records N size S match N different 0 errors 0` and exit 0;
3. `zpool status -v tank`: ONLINE, 0 READ, WRITE and CKSUM errors on every vdev, `errors: No known data errors`; and no `ereport` event for tank in `zpool events -v` since the run began;
4. the scratch pool as above (import, both keys, mount, `rec.py` on both files, export): exactly the two expected lines for each, with authentication events for gcdscr; give their count;
5. dmesg since the run began: no OOM kill, allocation failure, hung task, BUG or WARNING, and nothing from zfs or spl.

Leave M loaded, `tank` imported and idle, and `gcdscr` exported.

## Step 3: note to main, then wait for GO

Step 4 must run alone on the host, on a quiet VM: the three test VMs share one hypervisor, which also carries production load, and this task compares throughput and CPU time. You do not check the other VMs yourself; main does. End your turn with the interim results (the arms table as found: tree hashes, parents, log and numstat checks, checksums, configure crypto line, srcversions; the kit's checksums and the smoke test; the boot, cmdline and state lines; cpu.txt; the crypto_test summary lines and the ZTS result; the references and their read times; the CCM dataset or why it was skipped; the scratch pool's L0 lines and its result on M; what t2.sh's arguments do; the seq.sh diff) and, as the very last line, exactly:

`TASK-1118-GCD READY-TO-MEASURE (about 95 min alone on the host, at most 115; about 75 with NOCCM). Waiting for GO.`

No scheduled resumption of yours may be pending when you end that turn. Then do nothing until main writes. Go on to step 4 only on a message from main that contains `GO TASK-1118-GCD`; note the time of GO. If that message also contains `NOCCM`, skip the CCM runs (4b) and say so. On `HOLD`, keep waiting. On `ABORT TASK-1118-GCD`, go to END without running anything. Nothing runs while you wait (M loaded, `tank` imported and idle).

## Step 4: runs (alone on the host)

**Quiet check:** idle 60 s, then steal (from /proc/stat) and load1. If steal is above 2 % or load1 above 2 at the end, wait 2 minutes, run it once more, and go on either way, saying so.

**4a, AES-GCM runs,** `t2.sh <run> enc /tank/enc/f <tree> 600 10 600` as in AR, K35 and K35B, check `/tank/enc/f` against `$D/ref-enc.txt`, in this order (two passes, the second mirrored):

M-1, B-1, G-1, G-2, B-2, M-2

**4b, AES-CCM runs** (unless skipped), one per arm, `t2.sh <run> enc /tank/ccm/f <tree> 300 10 300` (or as step 1 decided), check `/tank/ccm/f` against `$D/ref-ccm.txt`:

CM-1, CB-1, CG-1

As in K35B, every run: `zpool export tank`, `scripts/zfs.sh -r` from the arm's tree, check both srcversions (stop on a mismatch), import `tank`, `load-key -a`, `mount -a`, steal and load1 at start and end, the UTC start and end, and stop on any OOM kill, allocation failure or hung task in dmesg; then check.sh. No control runs. Never drop a run; if one fails for a reason other than its check, rerun it once at the end and say so. A failed check is a stop, not a rerun: save `zpool status -v`, the tail of `zpool events -v`, dmesg and the check output under `$L`, and go to step 5 with what has run. Nothing else runs on this VM until the last run ends: no build, ZTS, ztest or second read job. Run seq.sh in your shell tool's own background mode (no `&`, no `nohup`) and read the tail of seq.log every few minutes.

When the last run has ended, write in your chat the line `TASK-1118-GCD MEASURED <UTC time>: the runs are over, the host is free.` and go on to step 5 without waiting.

## Step 5: report

Run AR's analysis on every run as K35 and K35B did (summ.sh, the one that produced the encrypted-run table, plus `dr-sum.py`), and `python3 $D/kit/k35-sum.py <outdir>/*.k35 > <outdir>/k35-sum.txt` over all the runs.

**CPU time per GiB,** per run, from `<run>.cpu` and `<run>.disk`: busy = user + nice + system + irq + softirq (fields 2, 3, 4, 7 and 8 of the `cpu` line; steal, idle and iowait excluded), in seconds (divide by `getconf CLK_TCK`); GiB read from the disks = Δ sectors read (field 6 of each diskstats line, summed over tank's disks) × 512 / 2^30. Every block read from the disks is decrypted once, so busy seconds per GiB read from the disks is the cost of reading and decrypting. Also give busy as a share of the window (busy / (window × nproc)), the user / system / softirq split, iowait, the GiB and MB/s read from the disks, and, if t2.sh gives the bytes its reads returned over the whole window, busy seconds per GiB returned.

**Builds and correctness:** the arms table as found (tree hashes, parents, log and numstat checks, checksums, configure and crypto lines, srcversions expected and loaded, warnings); the kit's checksums and the smoke test; cpu.txt and the ICP parameters of each load; crypto_test's summary lines; the ZTS result (or why skipped); the references and the scratch pool setup; per run, check.sh's result in one line (records matched, read time, tank's status and events, the scratch pool's two results and authentication count, dmesg).

**Runs,** one table for 4a, a column per run and the mean per arm, with K35B's rows:
- **read MB/s: first pass and mean of all passes** (the main metric);
- **CPU busy s per GiB read from the disks**, busy share, user / system / softirq, iowait; GiB and MB/s read from the disks;
- wake-ups of order > 0, by order (k35, and AR's trace count next to it);
- **vmap areas by size bucket** (k35-sum) and VmallocUsed start → end;
- kswapd runs; ARC shrinker requests by priority;
- `c` start / lowest / **mean** (MiB); first arrival at c_min; falls of `c` > 1 GiB and their kind; ARC data hits and misses (demand and prefetch, from `<run>.arcstats`);
- Δ allocstall_normal / movable, compact_stall, pgscan_direct, pageoutrun, memory_direct / indirect_count;
- dr.sh direct reclaim calls (total, total and max time), by kind of caller (decrypt, ABD, fault, other), and pages per call as AR gave them;
- order-0 kswapd wake-ups: total and top 3 gfp masks;
- the ICP implementations in use (`<run>.icp`);
- steal and load1, start → end, and the UTC start and end of the run.

**Wake-ups by request size and caller** (from `k35-sum.txt`), per run and per arm, as in K35:
- class × size bucket (<=32K, 32-128K, >128K) for all orders > 0, for order >= 4 and for order 8;
- by size bucket only, order >= 4; by class, first ZFS/ICP caller and task, order >= 4;
- the wake-ups whose size could not be found, with the reason; the vmap areas by size; the E counts per run;
- one full stack per class and arm, and the full stack of every wake-up of order >= 4 in G or CG.

**CCM runs (4b),** the same rows in a table of their own, one column per arm; with one run per arm there is no spread, so give the differences as an indication only.

**Differences larger than the spread.** For each row, a difference between two arm means counts only if it is larger than the spread (max − min) of each arm's two runs; otherwise write "no difference shown". Two runs per arm make this an indication, not a test. Give:
- **G against B,** on every row of the runs table, with both runs of each arm and each arm's spread;
- **B against M,** the same (expected: no difference shown);
- **G against M by class and bucket,** as K35 did for P against M.
- **Against earlier runs** of the same workload, for orientation only (other modules, other days): M against K35's M (order 8 1296 / 1194, mean `c` 2873 / 2643 MiB) and K35B's M; G against K35's O and PV and K35B's P2S (wake-ups of order >= 4, kswapd runs, mean `c`, direct reclaim, MB/s). If M disagrees with K35's M by more than their spread, say so before concluding.

**Host load:** you cannot see the other VMs. Give steal and load1 per run with the UTC times and the time of GO; say whether steal was above 0.1 % in any run.

**Answers,** one or two lines each with their numbers:
1. Does G remove the wake-ups of order >= 4 and the vmap areas > 128K of the decrypt path? Order >= 4 and vmap areas > 128K per run for M, B and G; any that remain in G, with class, bucket, order and stack.
2. G against B: read MB/s (first pass and mean), CPU busy s per GiB read from the disks, mean `c`, kswapd runs, allocstall_normal, pgscan_direct and direct reclaim calls with their max latency, with both runs and the spread of each arm, and the verdict by the rule above.
3. Does B match M on every row?
4. Correctness: crypto_test (pairs run, pairs not supported, failures), ZTS, every check.sh result, and the corrupted block on G (EIO, no data, authentication events).
5. CCM: order-8 wake-ups, vmap areas > 128K, MB/s and CPU s per GiB for CM, CB and CG.
6. **For the PR,** one line: B against G, read MB/s first pass / mean, wake-ups of order >= 4 per 600 s, mean `c` and CPU s per GiB, both runs of each.

## Expected (from reading the diffs and from K35 and K35B, not run)

| | M | B | G |
| --- | --- | --- | --- |
| order-8 wake-ups (K + VF + VD, >128K, decrypt) | about 1200–1300 per run (K35: 1296 / 1194) | as M | 0: the decrypt path no longer allocates |
| order 4–7 | 0 | 0 | 0 |
| order 1–3 | 0 to a few (K35's M: 0) | as M | 50–130 from non-SPL callers (copy_process, seq_read_iter, network), as K35's PV and O |
| vmap areas > 128K per run | about 274 000 (K35's M) | as M | near 0: none from decryption |
| kswapd runs | 5000–6000 | as M | a few hundred at most (K35's O: 568 / 690) |
| mean `c` | about 2.6–2.9 GiB | as M | at least 5.4 GiB (K35's O and PV) |
| allocstall_normal, direct reclaim calls | 0 | 0 | 0 to a few, from ABD allocations (K35's O: +2 / +8, part of them from the decrypt buffers G no longer has) |
| read MB/s first pass / mean | 476 / about 472 | as M | 476 / at least O's 492–505 if `c` is as large (the first pass has been 476 in every arm so far, probably what the disks give) |
| CPU busy s per GiB read from the disks | first measured here | as M | lower: per 1 MiB block, no 1 MiB kmalloc or vmalloc and no 1 MiB + 16 vmalloc (about 512 pages mapped and freed), 3 MiB less copied, 1 MiB less zeroed |
| CCM: order-8 wake-ups | from `ccm_init_ctx()` via `spl_kvmalloc()` (K, then VF): one 1 MiB allocation per block | as M | 0 |
| every check.sh | pass | pass | pass; the corrupted block gives EIO on every arm |

## END

- **Archive:** outputs (`.rec`, `.trace`, `.dr`, `.k35`, `.k35.stats`, `.k35.err`, `.cpu`, `.disk`, `.vmalloc`, `.arcstats`, `.icp`, `.check`, `k35-sum.txt`, logs, the analysis output, `seq.sh`, `seq.diff`, `check.sh`, `rec.py`, `seq.log`, steal, the smoke files, the references, `scr-ref.txt` and the zdb output, cpu.txt, crypto_test-G.txt, the ZTS result directory, the build logs, `tree-*.txt`, `sums-*.txt`, `log.txt`, `numstat.txt`) in `/var/log/task-1118-gcd/files.tar.gz`; then remove them, the extracted AR kit and `$D/kit` from `/var/tmp/task-1118-gcd`.
- **Remove:** the scratch pool (import it, `zpool destroy gcdscr`, then `$D/scr.img`, `$D/scr.key`, `$D/scr.data` and `/gcdscr` if left); `tank/ccm` (`zfs destroy tank/ccm`), then `zpool trim -w tank` so the thin storage gets the space back (if the disks do not support trim, say so and go on). Then export `tank` and unload the modules.
- **Keep:** the pool `tank` (exported) with `tank/enc` and its key; the trees `~/work/z-M0`, `~/work/z-M`, `~/work/z-F1`, `~/work/z-FA`, `~/work/z-P`, `~/work/z-PV`, `~/work/z-O`, `~/work/z-P2`, `~/work/z-P2S` and the new `~/work/z-B` and `~/work/z-G` (a follow-up may rerun them); the Proxmox kernel packages.
- **Restore:** `vm.compact_unevictable_allowed=1`, lru_gen 0x0007, swap as found.
- **Boot:** back into the Debian 6.12 kernel by a plain reboot (the default entry), resuming by your own arrangement; then `fstrim -av` and `df -h /`, and report the final state at the top as K35 did (kernel, next_entry, modules loaded, sysctl, swap, zpool trim, fstrim, free space, the reboots with their UTC times and how you resumed). Then send the full report as your last message.

## Stop conditions (all in one place)

Step 0's list; a fetch failing twice; a FETCH_HEAD other than the commits above; any commit, tree hash, parent, log, numstat, checksum or kit file that differs; rec.py's sha256 still differing after a transport fix; a failed build, configure's crypto line `yes`, a srcversion other than the table's, or two arms with the same zfs srcversion; a crypto_test failure, or a pair not supported although the CPU has its flags; a failed smoke test; the reference or the scratch pool not as expected on M; a srcversion mismatch at a load; a failed check.sh (then step 5 with what has run); an OOM kill, allocation failure or hung task in dmesg (save dmesg and stacks under `$L`, report; reboot only if the VM is wedged). For each stop, report what you see, then go to END for the parts that apply (remove the scratch pool and `tank/ccm`, restore, boot back to Debian). A result against the expectations is a finding, not a stop.
