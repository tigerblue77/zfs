#!/bin/bash
# Import every pool a scan finds in a copy of a kept crash state, in place
# or with the devices renamed and the recorded directory hidden.
# Usage: detpath_imp.sh <state.bak> <recorded-dir> <tree> inplace|renamed [bin-prefix]
# Prints one line per pool: [result] plus "tree:" lines when verbose (V=1).
E=$SCRATCH/pr/evidence
BAK=$1 REC=$2 T=$3 MODE=$4 BIN=${5:-bin_guidimp2}
W=/dev/shm/detpath/work.$$
rm -rf "${W:?}"
if [ "$MODE" = renamed ]; then
	mkdir "$W"
	for f in "$BAK"/vd.*; do cp -a "$f" "$W/m_$(basename "$f")"; done
	[ -e "$REC" ] && mv "$REC" "$REC.hid.$$"
	DIR=$W
else
	[ -e "$REC" ] && mv "$REC" "$REC.hid.$$"
	cp -a "$BAK" "$REC"
	DIR=$REC
fi
cd "$E/../$T" || exit 1
guids=$(./libtool --mode=execute "$E/${BIN}_$T" "$DIR" 0 2>&1 |
    awk '/^found pool guid/ {print $4}' | tr -d ',')
out=""
for g in $guids; do
	r=$(./libtool --mode=execute "$E/${BIN}_$T" "$DIR" "$g" 2>&1)
	[ -n "$V" ] && echo "$r" | grep -E "^tree:|vdev_copy|validate|missing|mismatch"
	s=$(echo "$r" | grep -E "^import of|^imported" | sed 's/.*: //;s/guid [0-9]*, root state //' | tr '\n' ' ')
	nbad=$(echo "$r" | grep "^tree:" | grep -E " (file|disk) " | grep -vc " state 7 ")
	out="$out[${s% }${nbad:+ badleaves=$nbad}]"
	# restore the devices for the next pool
	if [ "$MODE" = renamed ]; then
		rm -rf "${W:?}"; mkdir "$W"
		for f in "$BAK"/vd.*; do cp -a "$f" "$W/m_$(basename "$f")"; done
	else
		rm -rf "${REC:?}"; cp -a "$BAK" "$REC"
	fi
done
[ -z "$guids" ] && out="[no pool found]"
if [ "$MODE" = renamed ]; then
	rm -rf "${W:?}"
else
	rm -rf "${REC:?}"
fi
[ -e "$REC.hid.$$" ] && mv "$REC.hid.$$" "$REC"
echo "$out"
