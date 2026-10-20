#!/bin/bash
# usage: build_probe.sh <tree> <out> [extra CFLAGS]
# Builds empty_remove_probe.c against the given built tree (see probe_build.txt).
set -e
TREE=$1
OUT=$2
shift 2
SRC=$SCRATCH/pr/evidence/empty_remove_probe.c
cd "$TREE"
DBG=${PROBE_DEBUG_FLAGS:-'-DDEBUG -UNDEBUG -DZFS_DEBUG'}
CF='-DHAVE_CONFIG_H -I. -include ./zfs_config.h -include ./include/sys/simd_config.h -I./include -I./lib/libspl/include -I./lib/libspl/include/os/linux -I./lib/libzpool/include -D_GNU_SOURCE -D_REENTRANT -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE @DBG@ -std=gnu99 -Wall -Wextra -Wno-sign-compare -Wno-missing-field-initializers -fno-strict-aliasing -fno-omit-frame-pointer -pthread -Wno-format-zero-length -Wno-format-truncation -g -O2'
CF=${CF//@DBG@/$DBG}
gcc $CF "$@" -c -o "$OUT.o" "$SRC"
/bin/bash ./libtool --tag=CC --silent --mode=link gcc -pthread -g -O2 -o "$OUT" "$OUT.o" libzpool.la libzfs_core.la libnvpair.la -lm
