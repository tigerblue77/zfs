#!/bin/sh
# build.sh SRCTREE NAME: compile rzub.c against the libzpool of SRCTREE
set -e
B=$1
N=$2
R=$(cd "$(dirname "$0")" && pwd)
cd "$B"
gcc -DHAVE_CONFIG_H -I. -include ./zfs_config.h -include ./include/sys/simd_config.h -I./include -I./lib/libspl/include -I./lib/libspl/include/os/linux -I./lib/libzpool/include -D_GNU_SOURCE -D_REENTRANT -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE -DDEBUG -UNDEBUG -DZFS_DEBUG -DTEXT_DOMAIN=\"zfs-linux-user\" -std=gnu99 -Wall -Wextra -Wno-sign-compare -Wno-missing-field-initializers -Wno-unused-result -fno-strict-aliasing -fno-omit-frame-pointer -pthread -g -O2 -c -o "$R/rzub-$N.o" "$R/rzub.c"
/bin/bash ./libtool --tag=CC --silent --mode=link gcc -g -O2 -pthread -Wl,--export-dynamic -o "$R/rzub-$N" "$R/rzub-$N.o" libzpool.la libzfs_core.la libnvpair.la -lm -ldl
