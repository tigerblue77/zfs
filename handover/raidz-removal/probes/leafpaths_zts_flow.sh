#!/bin/bash
# Scratch (not for upstream): the dd steps of the detach ZTS tests with
# libzpool on file vdevs.  create mirror(vdev0, vdev1), export, copy, import
# from a scan, detach vdev1, export, then put back from the copy, on vdev0:
#   l0: all four labels and uberblock rings (crash right after the erase of
#       vdev1's labels, before any label of the detach txg: zpool_detach_003)
#   u:  labels 1 and 3 and the uberblock rings (crash right before the
#       uberblocks of the detach txg: zpool_detach_002 / 004)
# then import in place or with the devices renamed into another directory,
# print the vdev states and scrub.
# Usage: leafpaths_zts_flow.sh <gen-tree> <imp-tree> l0|u inplace|renamed [keep]
E=$SCRATCH/pr/evidence
G=$1 T=$2 P=$3 M=$4
B=/dev/shm/leafpaths/zts.$G.$T.$P.$M
rm -rf "${B:?}"; mkdir -p "$B/dev" "$B/pre"
cd "$E/../$G" || exit 1
./libtool --mode=execute "$E/bin_detpath_zts_$G" "$B/dev" create > /dev/null || exit 1
cp --sparse=always "$B"/dev/vdev* "$B/pre/"
./libtool --mode=execute "$E/bin_detpath_zts_$G" "$B/dev" detach | grep detach
n=$(( (256 << 20) / 131072 ))
if [ "$P" = l0 ]; then
	dd if="$B/pre/vdev0" of="$B/dev/vdev0" bs=128k skip=0 seek=0 count=4 conv=notrunc status=none
	dd if="$B/pre/vdev0" of="$B/dev/vdev0" bs=128k skip=$((n - 4)) seek=$((n - 4)) count=4 conv=notrunc status=none
else
	dd if="$B/pre/vdev0" of="$B/dev/vdev0" bs=128k skip=1 seek=1 count=3 conv=notrunc status=none
	dd if="$B/pre/vdev0" of="$B/dev/vdev0" bs=128k skip=$((n - 3)) seek=$((n - 3)) count=3 conv=notrunc status=none
fi
rm -f "$B/dev/zts.cache"
if [ "$M" = renamed ]; then
	mkdir "$B/moved"
	for f in "$B"/dev/vdev*; do mv "$f" "$B/moved/m_${f##*/}"; done
	DIR=$B/moved
else
	DIR=$B/dev
fi
if [ -n "$5" ]; then
	echo "kept: $DIR"
	exit 0
fi
cd "$E/../$T" || exit 1
g=$(./libtool --mode=execute "$E/bin_${GI:-guidimp2}_$T" "$DIR" 0 | awk '/^found pool guid/ {print $4}' | tr -d ',')
GUIDIMP_SCRUB=1 ./libtool --mode=execute "$E/bin_${GI:-guidimp2}_$T" "$DIR" "$g" 2>&1 | grep -E "^import of|^imported|^tree:|^scrub|^tryconfig|^found"
rm -rf "${B:?}"
