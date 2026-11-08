#!/bin/sh
# SPDX-License-Identifier: CDDL-1.0
# Bench only, not for upstream (issue #16503).
# Build the checksum bench against a configured and built userland tree,
# with xxh3_abd.c compiled once per xxHash vector path of this CPU family.
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
case $(uname -m) in
x86_64)	V="scalar:0: sse2:1: avx2:2:-mavx2 avx512:3:-mavx512f" ;;
aarch64) V="scalar:0: neon:4: sve:6:-march=armv8.2-a+sve" ;;
*)	echo "unsupported: $(uname -m)" >&2; exit 1 ;;
esac
objs=
for v in $V; do
	name=${v%%:*}
	rest=${v#*:}
	vec=${rest%%:*}
	flags=${rest#*:}
	# shellcheck disable=SC2086
	$CC $CPPFLAGS $CFLAGS $flags -DVARIANT="$name" -DXXH_VECTOR="$vec" \
	    -c -o "$O/xxh3_$name.o" "$H/xxh3_abd.c"
	objs="$objs $O/xxh3_$name.o"
done
# shellcheck disable=SC2086
$CC $CPPFLAGS $CFLAGS -c -o "$O/bench.o" "$H/bench.c"
# shellcheck disable=SC2086
$CC -pthread -o "$O/bench" "$O/bench.o" $objs -L"$T/.libs" \
    -Wl,-rpath,"$T/.libs" -lzpool -lnvpair -lm
echo "built $O/bench"
