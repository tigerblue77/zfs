<!--- Please fill out the following template, which will help other contributors review your Pull Request. -->

<!--- Provide a general summary of your changes in the Title above -->

### Motivation and Context
<!--- Why is this change required? What problem does it solve? -->
<!--- If it fixes an open issue, please link to the issue here. -->
#17058 imported BoringSSL's AES-GCM code for CPUs with VAES and VPCLMULQDQ, which works on 256-bit vectors (`avx2-vaes`). BoringSSL has a counterpart for 512-bit vectors, generated from `aes-gcm-avx512-x86_64.pl` and derived from the Linux kernel's AES-GCM code for AVX-512 (Eric Biggers). It handles 16 blocks per loop iteration instead of 8, and both BoringSSL and Linux select it over their 256-bit code on CPUs with AVX-512 that do not downclock: Intel Sapphire Rapids and later, AMD Zen 4 and later. This PR imports it the same way.

**With this change, `fastest` picks `avx512-vaes` on CPUs with VAES and AVX-512 outside the excluded models, that is Intel Sapphire Rapids and later and AMD Zen 4 and Zen 5**, mirroring Linux's choice and exclusion list. **This choice was not measured on real hardware**: I had no such CPU, and all testing below is correctness under Intel SDE. Reviewers with one of these CPUs are invited to compare the two implementations in user space with the existing benchmark mode of `crypto_test`, which times AES-GCM encryption of 1 KiB to 512 KiB for every implementation, including `aesni+avx2-vaes` and `aesni+avx512-vaes`:

```
/usr/share/zfs/zfs-tests/bin/crypto_test -p AES-GCM
```

(from a build tree: `./tests/zfs-tests/cmd/crypto_test -p AES-GCM`). In the kernel, the implementation can be switched with `echo avx2-vaes > /sys/module/zfs/parameters/icp_gcm_impl` (or `avx512-vaes`) before a workload on an encrypted dataset.

I found no existing issue or PR for this (searched for "avx512 gcm", "vaes avx512", "aes-gcm-avx512").

### Description
<!--- Describe your changes in detail -->
Two commits.

**Ship BoringSSL's Apache-2.0 licence with the ICP GCM code.** `aesni-gcm-avx2-vaes.S` comes from BoringSSL commit d5440dd and is tagged Apache-2.0, but `THIRDPARTYLICENSE.boringssl` (and its copy in `contrib/icp`) holds BoringSSL's older LICENSE file, from before BoringSSL moved to Apache-2.0: the OpenSSL, SSLeay and ISC texts, without the Apache License text. Both are replaced with BoringSSL's LICENSE at d5440dd. `THIRDPARTYLICENSE.boringssl` and its `.descrip` were also missing from `dist_noinst_DATA`, so release tarballs carried the assembly file but not its licence. This commit does not depend on the second one.

**Add a VAES AVX-512 AES-GCM implementation to the ICP.**

- `module/icp/asm-x86_64/modes/aesni-gcm-avx512-vaes.S` is BoringSSL's `gen/bcm/aes-gcm-avx512-x86_64-linux.S` at d5440dd, the same commit as the AVX2 file (the later BoringSSL commits to that perlasm only change comments and macOS/Windows output). It gets the same edits as the AVX2 file: AES rounds read at the ICP key schedule's offset and converted from 10/12/14, H byte-swapped as the ICP stores it, `ENTRY_ALIGN`/`SET_SIZE`, `ENDBR` and `RET` from `asm_linkage.h` (the kernel object has `endbr64` at each entry and its returns go through `__x86_return_thunk`; objtool reports nothing for it), `HAVE_SIMD()` guards, the GNU-stack note, and the unused gmult function dropped. Its symbols get the `icp_` prefix through `gcm_asm_rename_funcs.h`, since Linux's `arch/x86/crypto/aes-gcm-vaes-avx512.S` defines `aes_gcm_enc_update_vaes_avx512` and `aes_gcm_dec_update_vaes_avx512` too. The unmodified file is added to `contrib/icp/gcm-simd/boringssl` as a reference, with an SPDX line like its AVX2 neighbour.
- `CAN_USE_GCM_ASM` gets a level 3 when the toolchain also supports AVX512BW and AVX512VL; `GCM_IMPL_AVX512` is wired into `gcm.c` next to `GCM_IMPL_AVX2`. It uses the same 256-byte hash key table (BoringSSL's `u128 Htable[16]`, all of which the AVX-512 code uses) and the same `len & ~15` wrappers as BoringSSL's `hw_gcm_encrypt()`/`hw_gcm_decrypt()`.
- Selection: the code requires VAES, VPCLMULQDQ, AVX2, AVX512F, AVX512BW, AVX512VL and BMI2 (the perlasm header lists VAES, VPCLMULQDQ, AVX512BW, AVX512VL, BMI2). `icp_gcm_impl` lists it as `avx512-vaes` when the CPU has them. `fastest` picks it, except on CPUs where 512-bit vectors lower the clock frequency, where it picks `avx2-vaes` as before. The CPU list is BoringSSL's and Linux's: BoringSSL's `cpu_intel.cc` sets its "avoid zmm" bit on Intel family 6 models 85 (Skylake-X, Cascade Lake, Cooper Lake), 106 and 108 (Ice Lake server), 125 and 126 (Ice Lake client) and 140 and 141 (Tiger Lake), and Linux's `zmm_exclusion_list` has the same models plus Ice Lake NNPI (157), which this PR includes too. Linux 6.15 and later set `X86_FEATURE_PREFER_YMM` on them (torvalds/linux@968e9bc4cef8), which the new `zfs_prefer_ymm()` uses when defined; older kernels and user space match the models. AMD CPUs are never excluded. `avx512-vaes` can still be selected explicitly on the excluded CPUs.
- Register clearing: after the AVX-512 code has run, `clear_fpu_regs_avx512()` replaces `clear_fpu_regs_avx()` (still inside the `kfpu_begin()`/`kfpu_end()` section), adding 16 `vpxord` that zero zmm16-31. The ICP already clears the vector registers after use because they hold the round keys and hash key powers, but `vzeroall` does not reach zmm16-31, which the AVX-512 code uses for exactly those. BoringSSL and Linux do not do this, as neither clears the vector registers after use at all. The other implementations still call `clear_fpu_regs_avx()` only.
- `gcm_impl_will_work()` reported any value it did not know as usable, so `cycle` would select the new `GCM_IMPL_AVX512` in builds that have the AVX2 code but not the AVX-512 code. It now returns true only for the generic implementation and those compiled in.
- `crypto_test` asked for a GCM implementation named `avx2`, which does not exist (it is `avx2-vaes`), so it skipped the AVX2 code without failing; it now asks for `avx2-vaes` and `avx512-vaes`.

`man/man4/zfs.4` does not document `icp_gcm_impl`, so no man page change. FreeBSD's kernel build does not compile the ICP GCM assembly (`Makefile.bsd` enables neither AES nor VAES), so only Linux and user space are affected.

This PR should merge after #19411, which is needed for the hash key table, this implementation's included, to be wiped and freed with its size. #19407 makes the same `crypto_test` rename of `avx2` to `avx2-vaes`, so whichever of the two PRs merges second needs a trivial rebase.

Performance, measured on an Intel Core i7-1165G7 (Tiger Lake, one of the excluded models, so `fastest` keeps `avx2-vaes` there; `crypto_test` runs every implementation by name), userspace release build of this branch in a Debian 13 container on a lightly loaded host, `crypto_test -p AES-GCM`, average time per message, two runs:

```
avg encrypt (1000 rounds)        1K     2K     4K     8K    16K    32K    64K   128K   256K   512K
AES-GCM [aesni+avx]           934ns    1us    1us    2us    4us    8us   17us   33us   67us  136us
AES-GCM [aesni+avx2-vaes]     648ns  843ns    1us    1us    3us    6us   13us   26us   51us  104us
AES-GCM [aesni+avx512-vaes]   611ns  750ns  991ns    1us    2us    5us   10us   19us   39us   80us

AES-GCM [aesni+avx]           930ns    1us    1us    2us    4us    8us   17us   33us   67us  134us
AES-GCM [aesni+avx2-vaes]     646ns  834ns    1us    1us    3us    6us   13us   26us   51us  103us
AES-GCM [aesni+avx512-vaes]   622ns  746ns  992ns    1us    2us    5us   10us   20us   39us   79us
```

`avx512-vaes` takes about 23% less time than `avx2-vaes` from 128 KiB up (5% at 1 KiB). This single-threaded figure says nothing about what 512-bit code costs other work on these CPUs through frequency, which is why the exclusion list is kept as Linux has it. No measurement yet on a CPU where `fastest` changes (Sapphire Rapids and later, Zen 4 and Zen 5). Published figures for the equivalent code, none of them comparing 512-bit with 256-bit VAES code directly:

- BoringSSL commit 57f525e (Dec 2024, "Add VAES and VPCLMULQDQ accelerated AES-GCM"), AES-256-GCM encryption of 16384-byte messages, against BoringSSL's previous AES-NI/AVX code: 12726 vs 4761 MB/s on Sapphire Rapids, 13974 vs 5237 MB/s on Emerald Rapids, 10093 vs 4239 MB/s on Zen 4 (Genoa), 26358 vs 7194 MB/s on Zen 5.
- Linux commit b06affb1cb58 ("crypto: x86/aes-gcm - add VAES and AVX512 / AVX10 optimized AES-GCM"), against Linux's previous AES-NI/AVX2 code, AES-256-GCM encryption at 16384 bytes: +157% on Sapphire Rapids, +103% on Zen 4, +42% on Ice Lake (with 256-bit vectors there).
- Linux's later VAES+AVX2 patch ("crypto: x86/aes-gcm - remove VAES+AVX10/256 optimized code", Oct 2025) measured VAES+AVX2 within -6%..+4% of the 256-bit AVX-512 variant on Ice Lake server, which is why keeping `avx2-vaes` on the excluded CPUs costs little.
- For the AVX2 code already in OpenZFS, BoringSSL's commit 3b6e1be quoted up to 80% over AES-NI on Zen 3.

### How Has This Been Tested?
<!--- Please describe in detail how you tested your changes. -->
<!--- Include details of your testing environment, and the tests you ran to -->
<!--- see how your change affects other areas of the code, etc. -->
<!--- If your change is a performance enhancement, please provide benchmarks here. -->
<!--- Please think about using the draft PR feature if appropriate -->
Built the whole tree (debug build, `--enable-debug`), kernel modules against Linux 6.12.112 (`CONFIG_X86_KERNEL_IBT=y`, `CONFIG_MITIGATION_RETHUNK=y`, `CONFIG_OBJTOOL=y`) and user space, on x86_64 with GCC. Linux 6.12 has no `X86_FEATURE_PREFER_YMM`, so the kernel's model-list branch of `zfs_prefer_ymm()` was compiled; the `X86_FEATURE_PREFER_YMM` branch was not (no 6.15+ tree at hand). The kernel module was built but not loaded: the lab VM (QEMU TCG) has no AVX-512.

Correctness in user space only, with Intel SDE 10.13.1 emulating newer CPUs. SDE timings are meaningless and are not reported.

- `crypto_test -c aes_gcm_test.txt` (316 vectors, messages and AAD up to 513 bytes, 128/192/256-bit keys), run natively on the build host (Cascade Lake: AVX-512 but no VAES; all available implementations pass, `avx2-vaes`/`avx512-vaes` correctly unavailable) and under SDE `-tgl`, `-icl`, `-spr` (every implementation including `x86_64+avx512-vaes` and `aesni+avx512-vaes`: 316/316 passed) and `-adl` (no AVX-512: `avx512-vaes` correctly refused, `avx2-vaes` 316/316). Under SDE `-spr`, SDE's instruction mix shows all four `icp_*_avx512` functions executing. Before this change, the same run on master skipped the AVX2 rows (`couldn't enable GCM impl 'avx2'`). `crypto_test -c aes_ccm_test.txt` passes natively.
- A throwaway cross-check program (not part of this PR), run natively and under SDE `-spr`, `-tgl`, `-icl`: for every message length from 0 to 600 bytes plus lengths around 1 KiB, 4 KiB, the 32736-byte chunk size and its multiples up to 200003 bytes, AAD lengths from 0 to 40000 bytes, 128/192/256-bit keys and 12- and 16-byte IVs, every implementation (including `cycle` and `fastest`) must produce the same ciphertext and tag as generic AES with generic GCM, decrypt it back, and reject a corrupted tag. 0 failures.
- Register clearing, checked with a throwaway harness under SDE `-spr`, `-tgl` and `-icl` that dumps zmm0-31 and compares each 128-bit lane with the AES-256 round keys and the 16 hash key powers: right after `aes_gcm_enc_update_vaes_avx512`, all 32 registers are non-zero (52 lanes match); after `clear_fpu_regs_avx()` (`vzeroall`) zmm0-15 are zero but zmm16-31 are not (48 lanes still match); after `clear_fpu_regs_avx512()` all 32 are zero. After a full `crypto_encrypt()` through the ICP with `avx512-vaes`, zmm16-31 are zero and no lane matches a round key. SDE's instruction mix shows `clear_fpu_regs_avx512` executing in that path on `-spr` and only `clear_fpu_regs_avx` on `-tgl` (where `fastest` uses `avx2-vaes`).
- Implementation chosen by `fastest`, checked with SDE's instruction mix on a 64 KiB encryption: `avx512-vaes` on `-spr`; `avx2-vaes` on `-tgl` and `-icl` (model 0x8c and 0x7e, excluded) and on `-adl` (no AVX-512).
- ztest under SDE `-spr` (ztest encrypts half its datasets with a random suite and sets `icp_gcm_impl` to `cycle`): two 300-second runs (exit 0; one before and one after the register clearing was added) and a 90-second run in which SDE's instruction mix shows `icp_aes_gcm_enc_update_vaes_avx512`, `icp_aes_gcm_dec_update_vaes_avx512`, `icp_gcm_ghash_vpclmulqdq_avx512` and `icp_gcm_init_vpclmulqdq_avx512` executing alongside the AVX2 ones (exit 0).
- `cstyle.pl`, `spdxcheck.pl`, `commitcheck.sh` pass. `git diff --check` reports only the 11 lines of trailing whitespace of the verbatim BoringSSL reference copy in `contrib/icp`, as in its AVX2 neighbour.

On the same i7-1165G7, `crypto_test -c aes_gcm_test.txt` passes for all 12 implementations, `avx512-vaes` included.

Performance in user space, `crypto_test -p AES-GCM` (average time per encryption, run 1/run 2), on AMD EPYC 9V74 (Zen 4) and 9V45 (Zen 5), GitHub-hosted Ubuntu 24.04 runners (virtual machines), userspace release build:

```
                              4K           64K          512K
Zen 4  aesni+avx2-vaes     752/769 ns     9/9 us     72/72 us
       aesni+avx512-vaes   642/650 ns     7/7 us     58/61 us
Zen 5  aesni+avx2-vaes     618/620 ns     7/6 us     56/54 us
       aesni+avx512-vaes   434/417 ns     3/3 us     29/27 us
```

On Zen 5, which has full-width 512-bit units, `avx512-vaes` takes about half the time of `avx2-vaes` from 64 KiB up and 30% less at 4 KiB; on Zen 4 it takes 15-20% less. The model check excludes neither CPU, so `fastest` selects `avx512-vaes` on both. On both, `crypto_test -c aes_gcm_test.txt` passes for all 12 implementations.

Not tested: the kernel module at run time on any CPU with VAES, FreeBSD, Clang, the ZFS Test Suite.

### Types of Changes
<!--- What types of changes does your code introduce? Put an `x` in all the boxes that apply: -->
- [ ] Bug fix (non-breaking change which fixes an issue)
- [ ] New feature (non-breaking change which adds functionality)
- [x] Performance enhancement (non-breaking change which improves efficiency)
- [ ] Code cleanup (non-breaking change which makes code smaller or more readable)
- [ ] Quality assurance (non-breaking change which makes the code more robust against bugs)
- [ ] Breaking change (fix or feature that would cause existing functionality to change)
- [ ] Library ABI change (libzfs, libzfs\_core, libnvpair and libzfsbootenv)
- [ ] Documentation (a change to man pages or other documentation)

### Checklist
<!--- Go over all the following points, and put an `x` in all the boxes that apply. -->
<!--- If you're unsure about any of these, don't hesitate to ask. We're here to help! -->
- [x] My code follows the OpenZFS [code style requirements](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#coding-conventions).
- [ ] I have updated the documentation accordingly.
- [x] I have read the [**contributing** document](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md).
- [x] I have added [tests](https://github.com/openzfs/zfs/tree/master/tests) to cover my changes.
- [ ] I have run the ZFS Test Suite with this change applied.
- [x] All commit messages are properly formatted and contain [`Signed-off-by`](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#signed-off-by).
