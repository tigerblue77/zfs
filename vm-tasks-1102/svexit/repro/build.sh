#!/bin/sh
# Build vdev_exit_suspend against the libzpool of a built ZFS tree
# configured with --with-config=user --enable-debug.
# usage: build.sh <zfs tree> <output binary>
set -e
Z=$(cd "$1" && pwd -P)
SRC=$(cd "$(dirname "$0")" && pwd -P)/vdev_exit_suspend.c
OUT=$2
OBJ=$OUT.o
cd "$Z"
gcc -DHAVE_CONFIG_H -I. -include ./zfs_config.h \
    -include ./include/sys/simd_config.h -I./include \
    -I./lib/libspl/include -I./lib/libspl/include/os/linux \
    -I./lib/libzpool/include -D_GNU_SOURCE -D_REENTRANT \
    -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE -DDEBUG -UNDEBUG \
    -DZFS_DEBUG -DTEXT_DOMAIN=\"zfs-linux-user\" -std=gnu99 -Wall \
    -Wextra -Wstrict-prototypes -Wmissing-prototypes -Wwrite-strings \
    -Wno-sign-compare -Wno-missing-field-initializers \
    -fno-strict-aliasing -fno-omit-frame-pointer -Werror -pthread \
    -g -O2 -c -o "$OBJ" "$SRC"
./libtool --tag=CC --silent --mode=link gcc -pthread -g -O2 -o "$OUT" \
    "$OBJ" libzpool.la libzfs_core.la libnvpair.la -lm
