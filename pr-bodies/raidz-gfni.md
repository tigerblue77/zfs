<!--- Please fill out the following template, which will help other contributors review your Pull Request. -->

<!--- Provide a general summary of your changes in the Title above -->

### Motivation and Context
<!--- Why is this change required? What problem does it solve? -->
<!--- If it fixes an open issue, please link to the issue here. -->
RAID-Z multiplies bytes by constants of GF(2^8) with the polynomial 0x11d: by 2 and 4 when it generates Q and R, and by coefficients that depend on the missing columns when it reconstructs data. The x86 SIMD implementations do the first with compare/add/and/xor sequences and the second with four `pshufb` nibble lookups per byte: the AVX2 code takes 25 instructions to multiply two vectors by a constant and 8 to multiply them by 2.

GFNI's `GF2P8AFFINEQB` multiplies every byte of a vector by an 8x8 bit matrix, and multiplication by a constant is such a matrix in any GF(2^8), so one instruction per vector does either. (`GF2P8MULB` cannot be used: it is fixed to the AES polynomial 0x11b.) GFNI is present on Ice Lake and later Intel server and client cores, on Alder Lake and later hybrid parts (with VEX encodings only, since AVX-512 is off there), on Sierra Forest, and on AMD Zen 4 and later. I found no earlier issue or pull request about GFNI in OpenZFS.

### Description
<!--- Describe your changes in detail -->
Two new implementations, selectable through `zfs_vdev_raidz_impl` like the others and benchmarked by the module at load time:

- `avx2_gfni`: AVX2 + GFNI, VEX encodings, for GFNI CPUs without AVX-512.
- `avx512_gfni`: AVX-512F + GFNI, EVEX encodings. It needs no AVX512BW, since the only byte-granular operation is the affine one.

They differ from `avx2` and `avx512bw` only in `MUL2`, `MUL4` and `MUL`, so the rest of those two files moves, unchanged, into `vdev_raidz_math_avx2_common.h` and `vdev_raidz_math_avx512_common.h`, as `vdev_raidz_math_aarch64_neon_common.h` already does for the two NEON implementations; `vdev_raidz_math_avx2.c` and `vdev_raidz_math_avx512bw.c` keep their multiplication macros and registration, and compile to the same code as before (`objdump -d` of their objects is identical). In the GFNI files, `MUL2_SETUP()` broadcasts the matrices of 2 and 4 into two registers, `MUL2`/`MUL4` become one `vgf2p8affineqb` per register, and `MUL(c)` broadcasts the matrix of `c` and applies it. The 256 matrices are a `const` table, `gf_gfni_mul_lt`, next to `gf_clmul_mod_lt` in `vdev_raidz_math_ssse3.c`: column k of the matrix of c is c * 2^k, and row i (the input bits summed into output bit i) is byte 7 - i of the qword.

Plumbing:

- `config/toolchain-simd.m4`: a `GFNI` check of both the VEX (ymm) and EVEX (zmm) forms, giving `HAVE_TOOLCHAIN_GFNI` / `HAVE_KERNEL_GFNI`; `module/Makefile.bsd` lists the new sources and defines `HAVE_TOOLCHAIN_GFNI` like the other extensions.
- `zfs_gfni_available()` for the Linux kernel (`X86_FEATURE_GFNI`), FreeBSD (`CPUID_STDEXT2_GFNI`, present since FreeBSD 13) and userspace (CPUID leaf 7, ECX bit 8). The implementations also require `zfs_avx2_available()` or `zfs_avx512f_available()`, which check the OS-enabled register state.
- `gfni` in the `simd` kstat on Linux.
- `raidz_test` and `zfs(4)` list the new implementations.

Userspace consumers (`ztest`, `zdb`, `raidz_test` outside `-B`) do not benchmark: they take the last supported implementation, so they will use a GFNI one on CPUs that have GFNI. The FreeBSD in-tree build (`sys/modules/zfs`) will need the new sources and `-DHAVE_TOOLCHAIN_GFNI` at the next import. Tremont and Snow Ridge have GFNI without AVX; they would need an SSE variant, which this does not add.

### How Has This Been Tested?
<!--- Please describe in detail how you tested your changes. -->
<!--- Include details of your testing environment, and the tests you ran to -->
<!--- see how your change affects other areas of the code, etc. -->
<!--- If your change is a performance enhancement, please provide benchmarks here. -->
<!--- Please think about using the draft PR feature if appropriate -->
Measured on an Intel Core i7-1165G7 (Tiger Lake: AVX-512BW, GFNI), userspace release build (`--with-config=user`) of this branch in a Debian 13 container on a lightly loaded host, `raidz_test -B`, two runs; total bandwidth in GB/s for 1 MiB blocks (run 1/run 2):

```
impl               gen_p      gen_pq     gen_pqr       rec_p      rec_pq     rec_pqr
scalar         12.3/12.4     4.7/4.8     2.2/2.2   14.8/14.8     3.3/3.3     1.2/1.2
ssse3          17.5/17.4   11.0/11.0     7.9/7.9   21.6/21.6     6.6/6.6     2.9/2.9
avx2           26.0/26.0   17.1/16.9   12.8/12.7   30.0/30.8   16.3/16.2     9.5/9.5
avx512bw       27.1/26.8   19.3/19.2   14.8/14.7   30.8/31.4   19.6/19.4   12.1/12.1
avx2_gfni      26.3/25.9   17.8/17.5   13.9/13.9   30.5/30.8   18.6/18.6   13.3/13.2
avx512_gfni    26.8/26.8   19.3/19.2   15.2/15.2   30.7/31.4   20.3/19.8   14.6/14.5
```

The gain is where general multiplications dominate: triple-parity reconstruction (`rec_pqr`) is 20% faster with `avx512_gfni` than with `avx512bw`, and 39% faster with `avx2_gfni` than with `avx2`; `gen_pqr` and `rec_pq` gain 3% (AVX-512) and 9-14% (AVX2). Single parity and `gen_pq` use only XOR and multiplication by 2, and do not change. On the same machine, `raidz_test` (every implementation checked against the original one) passes. The kernel module has not been loaded on GFNI hardware yet.

The same measurement on AMD EPYC 9V74 (Zen 4) and 9V45 (Zen 5), on GitHub-hosted Ubuntu 24.04 runners (virtual machines, so noisier), same build, two runs each:

```
AMD EPYC 9V74 (Zen 4)
impl               gen_p      gen_pq     gen_pqr       rec_p      rec_pq     rec_pqr
avx2           40.2/40.7   26.7/26.0   21.0/20.9   47.7/45.8   27.4/27.3   16.0/15.7
avx512bw       40.9/42.0   28.1/28.0   22.6/22.6   48.0/48.3   27.2/28.2   15.9/15.4
avx2_gfni      40.4/40.4   28.0/28.2   23.3/23.4   48.0/47.8   31.3/30.6   23.2/22.9
avx512_gfni    41.3/41.5   28.1/28.1   22.9/22.8   47.7/47.3   29.8/30.8   22.2/21.4

AMD EPYC 9V45 (Zen 5)
impl               gen_p      gen_pq     gen_pqr       rec_p      rec_pq     rec_pqr
avx2           55.0/57.4   38.3/41.9   30.8/30.6   57.4/56.9   34.8/35.9   19.5/20.0
avx512bw       57.4/60.6   41.2/40.8   35.6/33.7   64.8/66.5   39.2/40.4   26.3/25.8
avx2_gfni      56.1/53.7   41.6/40.9   34.4/33.4   57.9/58.0   38.5/38.6   27.6/28.7
avx512_gfni    61.3/60.7   41.1/41.0   33.8/33.2   63.1/64.3   39.5/39.3   29.6/29.5
```

`rec_pqr` is again where it shows: 39% faster with `avx512_gfni` than with `avx512bw` on Zen 4 and 13% on Zen 5, and 36-45% faster with `avx2_gfni` than with `avx2` on both. With AVX2, `rec_pq` and `gen_pqr` gain 9-13%; with AVX-512 they change little. On Zen 4, which splits 512-bit operations in two, `avx2_gfni` is as fast as `avx512_gfni` or slightly faster; the kernel module picks the faster one by its benchmark, as for the other implementations. A second Zen 5 runner gave the same picture. `raidz_test` passes on all three runners.

Build: Linux 6.12.112 kernel module and userspace, debug build, GCC 13.3 and binutils 2.42 (Ubuntu 24.04). `configure` detects GFNI (both encodings) for the host and the kernel toolchains, and the warning count of the build is unchanged. `objdump -d` of `vdev_raidz_math_avx2.o` and `vdev_raidz_math_avx512bw.o` (kernel module, and libzpool PIC and non-PIC objects) is identical before and after the move into the shared headers, as are their non-debug sections. A userspace-only release build (`--with-config=user`) of a clean clone also passes `raidz_test -v`, `-v -e` and three short sweeps (`-S`, `-S -e`, `-S -e -r 0`) under SDE's Tiger Lake model.

Correctness under SDE, debug build, `raidz_test` (which checks every supported implementation against the original one):

| SDE CPU model | `avx2_gfni` | `avx512_gfni` | runs | result |
| --- | --- | --- | --- | --- |
| Tiger Lake (`-tgl`) | yes | yes | `-v`; `-v -e`; `-v -e -r 0`; `-v -d 1`; `-v -d 3 -a 9 -s 9`; `-v -d 13 -a 9 -s 20 -o 12`; `-v -d 16 -a 12 -s 17`; `-S -t 600`; `-S -e -t 600` | all pass; each sweep, capped at 600 s under emulation, covered 120 of its 165 maps |
| Sapphire Rapids (`-spr`) | yes | yes | `-v`; `-v -e`; `-v -d 16 -a 12 -s 17` | pass |
| Sierra Forest (`-srf`) | yes | no | `-v`; `-v -e`; `-v -d 16 -a 12 -s 17`; `-S -t 600` | pass; the sweep covered 134 maps |
| Tremont (GFNI without AVX), Skylake-SP | no | no | `-v` | pass, no SIGILL |

On top of `raidz_test`, whose reconstruction checks only reach the coefficients of the column counts they use, a small harness built against the tree multiplies buffers holding all 256 byte values by every constant 0..255 through each implementation's `raidz_mul_abd_cb()` (the real `MUL` code), comparing with a bitwise GF(2^8)/0x11d multiplication: 0 errors in 2,097,152 bytes per implementation under Tiger Lake, Sapphire Rapids and Sierra Forest, and 4062 errors per implementation when one bit of one matrix is flipped on purpose. A portable model of `GF2P8AFFINEQB` (as the Intel SDM defines it) agrees, for all 65,536 products, with `vdev_raidz_pow2`/`vdev_raidz_log2` and `gf_clmul_mod_lt`, and finds `gf_gfni_mul_lt` equal to the `gf_mul()` construction; it disagrees, as it should, when the matrix rows are reversed or the AES polynomial is used.

Natively on the build host (Xeon with AVX-512BW, no GFNI): `raidz_test -v`, `-v -e`, `-v -e -r 0` and a full `-S` sweep pass for the existing implementations, and both GFNI implementations are reported as not supported, without SIGILL.

Not tested: loading the kernel module (so neither the module's own benchmark, the `vdev_raidz_bench` kstat nor the `zfs_vdev_raidz_impl` listing), FreeBSD (kernel and userspace), and the ZFS Test Suite, of which only `raidz_001_neg` and `raidz_002_pos` run `raidz_test`.

### Types of Changes
<!--- What types of changes does your code introduce? Put an `x` in all the boxes that apply: -->
- [ ] Bug fix (non-breaking change which fixes an issue)
- [ ] New feature (non-breaking change which adds functionality)
- [x] Performance enhancement (non-breaking change which improves efficiency)
- [ ] Code cleanup (non-breaking change which makes code smaller or more readable)
- [ ] Quality assurance (non-breaking change which makes the code more robust against bugs)
- [ ] Breaking change (fix or feature that would cause existing functionality to change)
- [ ] Library ABI change (libzfs, libzfs\_core, libnvpair and libzfsbootenv)
- [x] Documentation (a change to man pages or other documentation)

### Checklist
<!--- Go over all the following points, and put an `x` in all the boxes that apply. -->
<!--- If you're unsure about any of these, don't hesitate to ask. We're here to help! -->
- [x] My code follows the OpenZFS [code style requirements](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#coding-conventions).
- [x] I have updated the documentation accordingly.
- [x] I have read the [**contributing** document](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md).
- [ ] I have added [tests](https://github.com/openzfs/zfs/tree/master/tests) to cover my changes.
- [ ] I have run the ZFS Test Suite with this change applied.
- [x] All commit messages are properly formatted and contain [`Signed-off-by`](https://github.com/openzfs/zfs/blob/master/.github/CONTRIBUTING.md#signed-off-by).
