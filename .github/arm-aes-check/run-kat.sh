#!/bin/sh
# SPDX-License-Identifier: CDDL-1.0
# Build and run the #18743 KAT/differential harness against a source tree.
# Bench/test only, not for upstream.
#   run-kat.sh <tree>                          native build and run (arm64)
#   CC="aarch64-linux-gnu-gcc -static" run-kat.sh <tree> qemu-aarch64 -cpu max
set -eu
T=$(cd "$1" && pwd); shift
H=$(cd "$(dirname "$0")" && pwd)
O=$(mktemp -d)
${CC:-cc} -O2 -I"$H/stub" -include "$T/include/sys/simd_config.h" \
    -DHAVE_TOOLCHAIN_ARMV8_AES=1 -DHAVE_TOOLCHAIN_ARMV8_PMULL=1 \
    -o "$O/kat" "$H/kat.c" \
    "$T/module/icp/algs/aes/aes_impl_armv8.c" \
    "$T/module/icp/algs/modes/gcm_pmull.c" \
    "$T/module/icp/algs/aes/aes_impl_generic.c" \
    "$T/module/icp/algs/modes/gcm_generic.c" \
    "$T/module/icp/asm-aarch64/aes/aesv8-armx.S" \
    "$T/module/icp/asm-aarch64/modes/ghashv8-armx.S"
rc=0
"$@" "$O/kat" || rc=$?
rm -rf "$O"
exit "$rc"
