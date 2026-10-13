#!/bin/sh
# SPDX-License-Identifier: CDDL-1.0
#
# Build a standalone libzpool test program against a built userspace tree
# (--with-config=user --enable-debug), with the flags zhack is built with.
#
#   build-tool.sh <built-tree> <source.c> <output>
#
# The tree's .libs/libzpool.so and friends must exist.  Nothing is written
# inside the tree.  The result is a plain executable: run it with
# LD_LIBRARY_PATH=<built-tree>/.libs so that it uses that tree's libzpool.
set -e
T=$(cd "$1" && pwd -P)
SRC=$(cd "$(dirname "$2")" && pwd -P)/$(basename "$2")
OUT=$(cd "$(dirname "$3")" && pwd -P)/$(basename "$3")
OBJ=$OUT.o
gcc -DHAVE_CONFIG_H -I"$T" -include "$T/zfs_config.h" \
    -include "$T/include/sys/simd_config.h" \
    -I"$T/include" -I"$T/lib/libspl/include" \
    -I"$T/lib/libspl/include/os/linux" -I"$T/lib/libzpool/include" \
    -D_GNU_SOURCE -D_REENTRANT -D_FILE_OFFSET_BITS=64 \
    -D_LARGEFILE64_SOURCE -DDEBUG -UNDEBUG -DZFS_DEBUG \
    -DTEXT_DOMAIN=\"zfs-linux-user\" \
    -std=gnu99 -Wall -Wextra -Wstrict-prototypes -Wmissing-prototypes \
    -Wwrite-strings -Wno-sign-compare -Wno-missing-field-initializers \
    -fno-strict-aliasing -fno-omit-frame-pointer -Wimplicit-fallthrough \
    -Werror -pthread -Wno-format-zero-length -Wno-format-truncation \
    -g -O1 -c -o "$OBJ" "$SRC"
gcc -g -O1 -pthread -o "$OUT" "$OBJ" -L"$T/.libs" \
    -lzpool -lzfs_core -lnvpair "$T/.libs/librange_tree.a" -lm
rm -f "$OBJ"
