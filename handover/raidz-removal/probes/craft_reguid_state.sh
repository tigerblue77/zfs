#!/bin/sh
# Turn a device of a pool exported after a reguid into what a crash right
# before the uberblocks of the reguid txg leaves: labels 1 and 3 and every
# uberblock ring come back from the copy taken before the reguid; labels 0
# and 2 keep the new guid.  Usage: craft_reguid_state.sh <pre-copy> <device>
set -e
pre=$1
dev=$2
n=$(( $(stat -c %s "$dev") / 131072 ))
dd if="$pre" of="$dev" bs=128k skip=1 seek=1 count=3 conv=notrunc 2>/dev/null
dd if="$pre" of="$dev" bs=128k skip=$((n - 3)) seek=$((n - 3)) count=3 \
    conv=notrunc 2>/dev/null
