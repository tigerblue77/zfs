#!/bin/sh
# SPDX-License-Identifier: CDDL-1.0
# Bench only, never for upstream.
# Build fl4bench against a configured and built userland tree.
#   build.sh <zfs tree> <output dir>
set -eu
T=$(cd "$1" && pwd)
mkdir -p "$2"
O=$(cd "$2" && pwd)
H=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-cc}
DEBUG_CPPFLAGS=$(sed -n 's/^DEBUG_CPPFLAGS = //p' "$T/Makefile")
CPPFLAGS="-include $T/zfs_config.h -include $T/include/sys/simd_config.h
    -I$T/include -I$T/lib/libspl/include -I$T/lib/libspl/include/os/linux
    -I$T/lib/libzpool/include -D_GNU_SOURCE -D_REENTRANT
    -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE $DEBUG_CPPFLAGS"
CFLAGS="-std=gnu99 -O2 -fno-strict-aliasing -pthread -Wall"
# shellcheck disable=SC2086
$CC $CPPFLAGS $CFLAGS -c -o "$O/fl4bench.o" "$H/fl4bench.c"
# shellcheck disable=SC2086
$CC -pthread -o "$O/fl4bench" "$O/fl4bench.o" -L"$T/.libs" \
    -Wl,-rpath,"$T/.libs" -lzpool -lnvpair -lm
echo "built $O/fl4bench"
