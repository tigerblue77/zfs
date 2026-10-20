#!/bin/bash
# Copy of ops_matrix.sh for the detach path fix.  For K in 1..N: kill
# ops_probe <op>, built against tree <gen>, right before the K-th label or
# uberblock write from the start of the operation to the end of the export.
# Then, with the master (wt-cf) and the fixed (wt-detpath) libzpool, import
# by guid every pool a scan finds, in place and with every device renamed
# and the recorded directory hidden, and open the pool from the cache file.
# Results: [import error, root state, leaves not healthy] per pool found.
# <op> split runs split_probe instead: the cache file is not opened.
# Usage: detpath_matrix.sh <scratch> <evidence> <op> <N> <gen>
S=$1
E=$2
OP=$3
N=$4
GEN=$5
COND=in_op
PROBE=(bin_ops_$GEN "$OP")
[ "$OP" = split ] && { COND=in_split; PROBE=(bin_split_$GEN); }
imp() {	# <tree> <dir>
	local t=$1 dir=$2 res="" g r s nbad
	cd "$E/../$t" || exit 1
	for g in $(./libtool --mode=execute "$E/bin_guidimp2_$t" "$dir" 0 2>&1 |
	    awk '/^found pool guid/ {print $4}' | tr -d ','); do
		r=$(./libtool --mode=execute "$E/bin_guidimp2_$t" "$dir" "$g" 2>&1)
		s=$(echo "$r" | grep -E "^import of|^imported" |
		    sed 's/.*: //;s/guid [0-9]*, root state //' | tr '\n' ' ')
		nbad=$(echo "$r" | grep "^tree:" | grep -E " (file|disk) " |
		    grep -vc " state 7 ")
		res="$res[${s% } bad=$nbad]"
		restore
	done
	echo "${res:-[none]}"
}
restore() {	# the devices in place, or renamed under $RN with $D hidden
	if [ -n "$RN" ]; then
		rm -rf "${RN:?}"; mkdir "$RN"
		for f in "$D.bak"/vd.*; do cp -a "$f" "$RN/m_${f##*/}"; done
	else
		rm -rf "${D:?}"; cp -a "$D.bak" "$D"
	fi
}
for K in $(seq 1 "$N"); do
	D=$S/${OP}_k$K
	RN=""
	rm -rf "${D:?}" "${D:?}.bak"
	mkdir -p "$D"
	cat > "$D.gdb" <<GDB
set pagination off
set confirm off
set breakpoint pending on
set \$n = 0
break vdev_label_sync_list if $COND
commands
silent
set \$n = \$n + 1
if \$n == $K
printf "KILL L%d:%llu\n", l, txg
kill
quit
end
continue
end
break vdev_uberblock_sync_list if $COND
commands
silent
set \$n = \$n + 1
if \$n == $K
printf "KILL U:%llu\n", ub->ub_txg
kill
quit
end
continue
end
run
quit
GDB
	where=$(cd "$E/../$GEN" && timeout 300 ./libtool --mode=execute gdb -q -batch \
	    -x "$D.gdb" --args "$E/${PROBE[0]}" "$D" ${PROBE[1]} 2>&1 | grep -m1 "^KILL")
	[ -z "$where" ] && { echo "$OP K=$K: no such write"; rm -rf "${D:?}" "$D.gdb"; break; }
	cp -a "$D" "$D.bak"
	line="$OP K=$K ${where#KILL }"
	for t in wt-cf wt-detpath; do
		RN=""
		inp=$(imp "$t" "$D")
		cd "$E/../$t" || exit 1
		c=$(./libtool --mode=execute "$E/bin_scanimp2_$t" "$D" op cache 2>&1 |
		    grep -E "^cache open|^opened" | sed 's/.*: //;s/guid [0-9]*, root state //' | tr '\n' ' ')
		restore
		RN=$S/rn.$$
		mv "$D" "$D.hid"
		restore
		rn=$(imp "$t" "$RN")
		rm -rf "${RN:?}"
		RN=""
		mv "$D.hid" "$D"
		line="$line | $t: in $inp ren $rn cache[${c% }]"
	done
	echo "$line"
done
