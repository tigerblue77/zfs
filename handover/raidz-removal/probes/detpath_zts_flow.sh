#!/bin/bash
# The steps of the detach ZTS test with libzpool: create mirror(vdev0,
# vdev1), export, copy, import, detach vdev1, export, put back labels 1 and
# 3 and the uberblock rings of vdev0 from the copy, then import in place
# or with the devices renamed.  Usage: detpath_zts_flow.sh <gen-tree> <imp-tree> inplace|renamed
E=$SCRATCH/pr/evidence
G=$1 T=$2 M=$3
B=/dev/shm/detpath/zts.$G.$T.$M
rm -rf "${B:?}"; mkdir -p "$B/dev" "$B/pre"
cd "$E/../$G" || exit 1
./libtool --mode=execute "$E/bin_detpath_zts_$G" "$B/dev" create > /dev/null || exit 1
cp --sparse=always "$B"/dev/vdev* "$B/pre/"
./libtool --mode=execute "$E/bin_detpath_zts_$G" "$B/dev" detach | grep detach
n=$(( (256 << 20) / 131072 ))
dd if="$B/pre/vdev0" of="$B/dev/vdev0" bs=128k skip=1 seek=1 count=3 conv=notrunc status=none
dd if="$B/pre/vdev0" of="$B/dev/vdev0" bs=128k skip=$((n - 3)) seek=$((n - 3)) count=3 conv=notrunc status=none
rm -f "$B/dev/zts.cache"
if [ "$M" = renamed ]; then
	mkdir "$B/moved"
	for f in "$B"/dev/vdev*; do mv "$f" "$B/moved/m_${f##*/}"; done
	DIR=$B/moved
else
	DIR=$B/dev
fi
cd "$E/../$T" || exit 1
g=$(./libtool --mode=execute "$E/bin_${GI:-guidimp2}_$T" "$DIR" 0 | awk '/^found pool guid/ {print $4}' | tr -d ',')
./libtool --mode=execute "$E/bin_${GI:-guidimp2}_$T" "$DIR" "$g" 2>&1 | grep -E "^import of|^imported|^tree:|^scrub"
rm -rf "${B:?}"
